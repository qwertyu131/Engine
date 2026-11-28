#pragma once
// CPU glTF model (plan F3 Task 2): plain data, no GPU types. GltfLoader.cpp fills it from fastgltf's parse;
// every vertex stream is decoded (normalized integers and sparse accessors resolved) and bounds-checked.
//
// Conventions:
//   - matrices are column-major float[16] (glTF order); quaternions (x, y, z, w);
//   - indices into the arrays of ModelData, -1 = none;
//   - every primitive has a uint32 index list (generated 0..n-1 when the file has none); LINE_LOOP and
//     TRIANGLE_FAN are converted to LineList / TriangleList here, so Topology has no loop / fan;
//   - streams are either empty (attribute absent) or exactly vertexCount long.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace eng::asset {

using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;
using Mat4 = std::array<float, 16>; // column-major

enum class Topology : uint8_t { PointList, LineList, LineStrip, TriangleList, TriangleStrip };

struct Bounds {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};
    bool valid = false; // false for an empty primitive
};

// Morph target deltas (each empty or vertexCount long).
struct MorphTarget {
    std::vector<Vec3> position;
    std::vector<Vec3> normal;
    std::vector<Vec3> tangent;
};

inline constexpr uint32_t kMaxTexCoords = 4; // TEXCOORD_0..3 kept; more are ignored with a warning

struct Primitive {
    Topology topology = Topology::TriangleList;
    uint32_t vertexCount = 0;
    std::vector<Vec3> position;
    std::vector<Vec3> normal;
    std::vector<Vec4> tangent; // xyz + handedness w (+-1)
    std::array<std::vector<Vec2>, kMaxTexCoords> texCoord;
    std::vector<Vec4> color0;  // VEC3 colours get alpha 1
    std::array<std::vector<std::array<uint16_t, 4>>, 2> joints; // JOINTS_0 / JOINTS_1
    std::array<std::vector<Vec4>, 2> weights;                   // WEIGHTS_0 / WEIGHTS_1
    std::vector<uint32_t> indices;
    std::vector<MorphTarget> targets;
    int material = -1; // -1 = the glTF default material
    Bounds bounds;     // of `position`
    // The POSITION accessor's min / max from the JSON (dequantized when normalized), as glTF-Sample-Renderer's
    // getExtentsFromAccessor reads them for the fit camera (plan F3 Task 6); invalid when the accessor has none.
    Bounds accessorBounds;
    // What the loader generated (Review Focus 4): flat normals (no NORMAL), MikkTSpace tangents (no TANGENT with a
    // normal map), a 0..n-1 index list (no indices), a zero UV set (a texture's TEXCOORD_n missing).
    bool generatedNormals = false;
    bool generatedTangents = false;
    bool generatedIndices = false;
    bool generatedTexCoord = false;
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
    std::vector<float> weights; // default morph weights
};

struct Node {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    bool hasMatrix = false;
    Mat4 matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    int mesh = -1;
    int camera = -1;
    int skin = -1;
    int light = -1; // KHR_lights_punctual
    std::vector<float> weights;
};

struct Scene {
    std::string name;
    std::vector<int> nodes; // root nodes
};

enum class ColorSpace : uint8_t { Linear, Srgb };

struct TextureRef {
    int image = -1;   // ModelData::images
    int sampler = -1; // ModelData::samplers, -1 = default (repeat, linear, mipmapped)
    uint32_t texCoord = 0;
    float scale = 1.0f; // normalTexture.scale / occlusionTexture.strength, else 1
    bool valid() const { return image >= 0; }
};

enum class AlphaMode : uint8_t { Opaque, Mask, Blend };

struct Material {
    std::string name;
    Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    Vec3 emissiveFactor{0.0f, 0.0f, 0.0f};
    TextureRef baseColor;         // sRGB
    TextureRef metallicRoughness; // linear
    TextureRef normal;            // linear
    TextureRef occlusion;         // linear
    TextureRef emissive;          // sRGB
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    std::string extensionsJson; // the material's raw "extensions" object ("" if none), for F6
};

struct Image {
    std::string name;
    std::string mimeType;  // image/png, image/jpeg, image/ktx2, image/webp, ...
    std::string uri;       // external file / data-URI marker ("" for bufferView images)
    // Usage (plan F3 Review Focus 3): baseColor / emissive -> sRGB, the other slots -> linear. `colorSpace` is sRGB
    // when any material uses the image as sRGB; both flags set = used both ways (warned).
    ColorSpace colorSpace = ColorSpace::Linear;
    bool usedAsSrgb = false;
    bool usedAsLinear = false;
    bool decoded = false;          // rgba8 holds the pixels
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba8;    // decoded PNG / JPEG / WebP, rows top-down
    // KHR_texture_basisu (plan F3 Task 3): `encoded` is the whole KTX2 file (Basis Universal ETC1S or UASTC payload),
    // kept compressed and transcoded at upload to a format the device samples (Ktx2.h). width / height are those of
    // level ktx2BaseLevel (r.Asset.MaxTextureSize skips larger levels), ktx2Levels the mip levels from there on.
    // ktx2Srgb = the DFD transfer function is sRGB: it decides the GPU view (sRGB or UNORM), not the usage flags.
    bool ktx2 = false;
    bool ktx2Srgb = false;
    uint32_t ktx2BaseLevel = 0;
    uint32_t ktx2Levels = 0;
    std::vector<uint8_t> encoded;
};

enum class Filter : uint8_t { Nearest, Linear };
enum class MipFilter : uint8_t { None, Nearest, Linear };
enum class Wrap : uint8_t { Repeat, ClampToEdge, MirroredRepeat };

struct Sampler {
    Filter magFilter = Filter::Linear;
    Filter minFilter = Filter::Linear;
    MipFilter mipFilter = MipFilter::Linear;
    Wrap wrapS = Wrap::Repeat;
    Wrap wrapT = Wrap::Repeat;
};

struct Camera {
    enum class Type : uint8_t { Perspective, Orthographic };
    std::string name;
    Type type = Type::Perspective;
    float yfov = 0.0f;        // perspective, radians
    float aspectRatio = 0.0f; // perspective, 0 = the viewport's
    float xmag = 0.0f, ymag = 0.0f; // orthographic
    float znear = 0.0f;
    float zfar = 0.0f;        // 0 = infinite (perspective only)
};

struct Skin {
    std::string name;
    std::vector<int> joints;
    std::vector<Mat4> inverseBindMatrices; // joints.size() entries (identity when the file has none)
    int skeleton = -1;
};

struct AnimationSampler {
    enum class Interpolation : uint8_t { Linear, Step, CubicSpline };
    Interpolation interpolation = Interpolation::Linear;
    std::vector<float> input;  // key times
    std::vector<float> output; // decoded to float, `outputComponents` per element
    uint32_t outputComponents = 0;
};

struct AnimationChannel {
    enum class Path : uint8_t { Translation, Rotation, Scale, Weights, Pointer };
    int sampler = -1;
    int node = -1;
    Path path = Path::Translation;
    std::string pointer; // KHR_animation_pointer (F4)
};

struct Animation {
    std::string name;
    std::vector<AnimationChannel> channels;
    std::vector<AnimationSampler> samplers;
};

struct Light {
    enum class Type : uint8_t { Directional, Point, Spot };
    std::string name;
    Type type = Type::Point;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 0.0f; // 0 = infinite
    float innerConeAngle = 0.0f;
    float outerConeAngle = 0.785398163f; // pi / 4
};

struct ModelData {
    std::vector<Scene> scenes;
    int defaultScene = -1; // gltf.scene, -1 when absent (the renderer then uses scene 0)
    std::vector<Node> nodes;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Image> images;
    std::vector<Sampler> samplers;
    std::vector<Camera> cameras;
    std::vector<Skin> skins;
    std::vector<Animation> animations; // parsed, played in F4
    std::vector<Light> lights;         // KHR_lights_punctual
    std::vector<std::string> extensionsUsed;
    std::vector<std::string> extensionsRequired;
};

} // namespace eng::asset
