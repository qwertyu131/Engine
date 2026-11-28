#pragma once
// Primitive post-processing of the loader (plan F3 Task 2, Review Focus 4), exposed for the asset tests.
#include "ModelData.h"

#include <cstdint>
#include <string>

namespace eng::asset {

// Strips become lists (TriangleStrip -> TriangleList with alternating winding fixed, LineStrip -> LineList).
void stripToList(Primitive& p);

// Every index gets its own vertex (all streams and morph targets copied); indices become 0..n-1. Strips are
// converted to lists first.
void deindex(Primitive& p);

// Flat normals (glTF 2.0 §3.7.2.1: "client implementations MUST calculate flat normals"): de-indexes, then each
// triangle's vertices get its face normal (counter-clockwise front). Triangle topologies only; no-op otherwise.
void generateFlatNormals(Primitive& p);

// MikkTSpace tangents (Tangents.cpp) from NORMAL and TEXCOORD_<texCoord>: de-indexes, then one tangent per vertex
// (xyz + handedness w = +-1, glTF convention bitangent = cross(normal, tangent) * w). False (with the reason in
// `why`) for a non-triangle primitive or a missing NORMAL / UV set.
bool generateTangents(Primitive& p, uint32_t texCoord, std::string* why);

} // namespace eng::asset
