#include "DeviceCaps.h"

#include <nlohmann/json.hpp>

namespace eng::rhi {

const FormatCaps& DeviceCaps::format(Format f) const {
    static const FormatCaps kNone;
    for (const auto& [format, caps] : formatSupport) {
        if (format == f) {
            return caps;
        }
    }
    return kNone;
}

void forEachCapsField(const DeviceCaps& c, const std::function<void(std::string_view, const CapsValue&)>& fn) {
    auto num = [&](std::string_view name, uint64_t v) { fn(name, CapsValue(static_cast<int64_t>(v))); };
    auto flag = [&](std::string_view name, bool v) { fn(name, CapsValue(v)); };
    fn("backend", CapsValue(std::string(toString(c.backend))));
    fn("tier", CapsValue(static_cast<int64_t>(c.tier)));
    fn("apiVersion", CapsValue(c.apiVersion));
    fn("adapterName", CapsValue(c.adapterName));
    num("vendorId", c.vendorId);
    flag("software", c.software);
    num("maxTexture2D", c.maxTexture2D);
    num("maxCubeSize", c.maxCubeSize);
    num("maxColorAttachments", c.maxColorAttachments);
    num("maxVertexAttributes", c.maxVertexAttributes);
    num("maxUniformBufferBytes", c.maxUniformBufferBytes);
    num("maxBindingsPerStage.cbuffer", c.maxBindingsPerStage.cbuffer);
    num("maxBindingsPerStage.texture", c.maxBindingsPerStage.texture);
    num("maxBindingsPerStage.sampler", c.maxBindingsPerStage.sampler);
    num("maxBindingsPerStage.storage", c.maxBindingsPerStage.storage);
    flag("computeShaders", c.computeShaders);
    flag("storageBuffers", c.storageBuffers);
    flag("vertexStorageBuffers", c.vertexStorageBuffers);
    for (const auto& [format, caps] : c.formatSupport) {
        fn(std::string("formatSupport.") + toString(format), CapsValue(caps));
    }
    flag("bc", c.bc);
    flag("bc6h7", c.bc6h7);
    flag("etc2", c.etc2);
    flag("astc", c.astc);
    num("msaaSampleCounts", c.msaaSampleCounts);
    flag("float16", c.float16);
    flag("waveOps", c.waveOps);
    flag("rayTracing", c.rayTracing);
    flag("bindless", c.bindless);
    flag("timestampQueries", c.timestampQueries);
}

std::string toString(const FormatCaps& f) {
    std::string s;
    auto add = [&](bool on, const char* name) {
        if (on) {
            s += (s.empty() ? "" : " ");
            s += name;
        }
    };
    add(f.sample, "sample");
    add(f.filter, "filter");
    add(f.render, "render");
    add(f.blend, "blend");
    add(f.depth, "depth");
    add(f.vertex, "vertex");
    return s.empty() ? "-" : s;
}

std::string capsToJson(const DeviceCaps& caps, std::string_view note) {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    forEachCapsField(caps, [&](std::string_view name, const CapsValue& v) {
        nlohmann::ordered_json* node = &j;
        std::string_view rest = name;
        for (size_t dot = rest.find('.'); dot != std::string_view::npos; dot = rest.find('.')) {
            node = &(*node)[std::string(rest.substr(0, dot))];
            rest.remove_prefix(dot + 1);
        }
        nlohmann::ordered_json& leaf = (*node)[std::string(rest)];
        if (const bool* b = std::get_if<bool>(&v)) {
            leaf = *b;
        } else if (const int64_t* i = std::get_if<int64_t>(&v)) {
            leaf = *i;
        } else if (const std::string* str = std::get_if<std::string>(&v)) {
            leaf = *str;
        } else {
            const FormatCaps& f = std::get<FormatCaps>(v);
            leaf = {{"sample", f.sample}, {"filter", f.filter}, {"render", f.render},
                    {"blend", f.blend},   {"depth", f.depth},   {"vertex", f.vertex}};
        }
    });
    if (!note.empty()) {
        j["note"] = std::string(note);
    }
    return j.dump(2) + "\n";
}

} // namespace eng::rhi
