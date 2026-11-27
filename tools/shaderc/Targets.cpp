#include "Targets.h"

namespace eng::sc {

std::string toString(Target t) {
    switch (t) {
    case Target::DXIL: return "dxil";
    case Target::DXBC: return "dxbc";
    case Target::SPIRV: return "spirv";
    case Target::MSL: return "msl";
    case Target::GLSL_ES300: return "glsl-es300";
    case Target::GLSL_ES320: return "glsl-es320";
    case Target::WGSL: return "wgsl";
    }
    return "?";
}

std::string toString(Stage s) {
    switch (s) {
    case Stage::Vertex: return "vs";
    case Stage::Fragment: return "fs";
    case Stage::Compute: return "cs";
    }
    return "?";
}

std::string toString(DxbcPath p) {
    return p == DxbcPath::Direct ? "direct" : "cross";
}

std::optional<Target> parseTarget(std::string_view s) {
    for (Target t : kAllTargets) {
        if (toString(t) == s) {
            return t;
        }
    }
    return std::nullopt;
}

std::optional<Stage> parseStage(std::string_view s) {
    for (Stage st : {Stage::Vertex, Stage::Fragment, Stage::Compute}) {
        if (toString(st) == s) {
            return st;
        }
    }
    return std::nullopt;
}

std::optional<DxbcPath> parseDxbcPath(std::string_view s) {
    if (s == "direct") {
        return DxbcPath::Direct;
    }
    if (s == "cross") {
        return DxbcPath::Cross;
    }
    return std::nullopt;
}

std::string extension(Target t) {
    switch (t) {
    case Target::DXIL: return "dxil";
    case Target::DXBC: return "dxbc";
    case Target::SPIRV: return "spv";
    case Target::MSL: return "metal";
    case Target::GLSL_ES300:
    case Target::GLSL_ES320: return "glsl";
    case Target::WGSL: return "wgsl";
    }
    return "bin";
}

bool isTextTarget(Target t) {
    return t == Target::MSL || t == Target::GLSL_ES300 || t == Target::GLSL_ES320 || t == Target::WGSL;
}

} // namespace eng::sc
