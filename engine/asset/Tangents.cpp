// MikkTSpace tangent generation (plan F3 Task 2, Review Focus 4): glTF 2.0 §3.7.2.1 "When tangents are not
// specified, client implementations SHOULD calculate tangents using default MikkTSpace algorithms with the
// specified vertex positions, normals, and texture coordinates associated with the normal texture."
// The primitive is de-indexed first (MikkTSpace may give one shared vertex different tangents per face), so face f
// vertex v is vertex 3 f + v.
#include "Geometry.h"

#include <mikktspace.h>

#include <cmath>

namespace eng::asset {

namespace {

struct MikkData {
    Primitive* p;
    uint32_t uv;
};

Primitive& prim(const SMikkTSpaceContext* ctx) {
    return *static_cast<MikkData*>(ctx->m_pUserData)->p;
}

int numFaces(const SMikkTSpaceContext* ctx) {
    return static_cast<int>(prim(ctx).vertexCount / 3u);
}

int numVerticesOfFace(const SMikkTSpaceContext*, const int) {
    return 3;
}

void getPosition(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const Vec3& v = prim(ctx).position[static_cast<size_t>(face) * 3u + static_cast<size_t>(vert)];
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
}

void getNormal(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const Vec3& v = prim(ctx).normal[static_cast<size_t>(face) * 3u + static_cast<size_t>(vert)];
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
}

void getTexCoord(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert) {
    const auto* d = static_cast<MikkData*>(ctx->m_pUserData);
    const Vec2& v = d->p->texCoord[d->uv][static_cast<size_t>(face) * 3u + static_cast<size_t>(vert)];
    out[0] = v[0];
    out[1] = v[1];
}

void setTSpaceBasic(const SMikkTSpaceContext* ctx, const float tangent[], const float sign, const int face,
                    const int vert) {
    // glTF texture space has v pointing down (UV (0, 0) = top-left), MikkTSpace assumes v up: the handedness flips,
    // so w = -fSign (as three.js computeMikkTSpaceTangents(..., negateSign = true) documents for glTF; matches the
    // TANGENT data of NormalTangentMirrorTest, see test_loader.cpp).
    prim(ctx).tangent[static_cast<size_t>(face) * 3u + static_cast<size_t>(vert)] = {tangent[0], tangent[1], tangent[2],
                                                                                     sign < 0.0f ? 1.0f : -1.0f};
}

} // namespace

bool generateTangents(Primitive& p, uint32_t texCoord, std::string* why) {
    const auto reason = [&](const char* r) {
        if (why) {
            *why = r;
        }
        return false;
    };
    if (p.topology != Topology::TriangleList && p.topology != Topology::TriangleStrip) {
        return reason("not a triangle primitive");
    }
    if (p.normal.empty()) {
        return reason("no NORMAL");
    }
    if (texCoord >= kMaxTexCoords || p.texCoord[texCoord].empty()) {
        return reason("no TEXCOORD set of the normal texture");
    }
    deindex(p); // vertexCount is a multiple of 3 afterwards (a dangling partial triangle is dropped)
    p.tangent.assign(p.vertexCount, Vec4{1.0f, 0.0f, 0.0f, 1.0f});
    MikkData data{&p, texCoord};
    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = numFaces;
    iface.m_getNumVerticesOfFace = numVerticesOfFace;
    iface.m_getPosition = getPosition;
    iface.m_getNormal = getNormal;
    iface.m_getTexCoord = getTexCoord;
    iface.m_setTSpaceBasic = setTSpaceBasic;
    SMikkTSpaceContext ctx{};
    ctx.m_pInterface = &iface;
    ctx.m_pUserData = &data;
    if (p.vertexCount > 0 && genTangSpaceDefault(&ctx) == 0) {
        return reason("MikkTSpace failed (out of memory)");
    }
    for (Vec4& t : p.tangent) {
        if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) {
            t = {1.0f, 0.0f, 0.0f, 1.0f};
        }
    }
    p.generatedTangents = true;
    return true;
}

} // namespace eng::asset
