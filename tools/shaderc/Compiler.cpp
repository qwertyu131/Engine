#include "Compiler.h"

#include "Process.h"

#include <spirv_glsl.hpp>
#include <spirv_hlsl.hpp>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace eng::sc {

std::string toolNotFound(std::string_view name) {
    return "tool not found: " + std::string(name) + " (run scripts/fetch_shader_tools.py)";
}

std::expected<fs::path, std::string> requireTool(const fs::path& path, std::string_view name) {
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec)) {
        return std::unexpected(toolNotFound(name));
    }
    return path;
}

namespace {

constexpr uint32_t kGlslBindingsPerSpace = 16; // GLSL ES 3.20 flattened binding = space * 16 + index

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

std::string profile(Stage s, const char* sm) {
    const char* p = s == Stage::Vertex ? "vs" : s == Stage::Fragment ? "ps" : "cs";
    return std::string(p) + "_" + sm;
}

spv::ExecutionModel executionModel(Stage s) {
    switch (s) {
    case Stage::Vertex: return spv::ExecutionModelVertex;
    case Stage::Fragment: return spv::ExecutionModelFragment;
    case Stage::Compute: return spv::ExecutionModelGLCompute;
    }
    return spv::ExecutionModelMax;
}

std::vector<uint32_t> toWords(const std::vector<uint8_t>& bytes) {
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}

class Job {
public:
    Job(const CompileRequest& req, const ToolPaths& tools) : req_(req), tools_(tools) {}

    std::expected<CompileResult, std::string> run();

private:
    // DXC with the request's entry/profile/defines/includes; returns the -Fo output.
    std::expected<std::vector<uint8_t>, std::string> dxc(const std::vector<Arg>& extra, const std::string& outName,
                                                         bool entryAndProfile = true);
    std::expected<std::vector<uint8_t>, std::string> fxc(const fs::path& hlsl, const std::string& entry);
    std::expected<void, std::string> tool(const fs::path& exe, const char* name, const std::vector<Arg>& args);

    std::expected<void, std::string> toSpirv(CompileResult& out);
    std::expected<void, std::string> toDxil(CompileResult& out);
    std::expected<void, std::string> toWgsl(CompileResult& out);
    std::expected<void, std::string> toMsl(CompileResult& out);
    std::expected<void, std::string> toGlsl(CompileResult& out, uint32_t version);
    std::expected<void, std::string> toDxbcCross(CompileResult& out);
    std::expected<void, std::string> toDxbcDirect(CompileResult& out);
    std::expected<void, std::string> checkTargetFeatures() const;

    const CompileRequest& req_;
    const ToolPaths& tools_;
    TempDir tmp_;
    std::string preprocessed_; // DXC -P output: every declaration, includes and defines resolved
    std::vector<uint8_t> spirvBytes_;
    std::vector<uint32_t> spirv_;
    Reflection refl_;
    std::vector<std::string> diagnostics_;
};

std::expected<void, std::string> Job::tool(const fs::path& exe, const char* name, const std::vector<Arg>& args) {
    auto r = runProcess(exe, args, tmp_.path());
    if (!r) {
        return std::unexpected(r.error());
    }
    std::string output = trim(r->output);
    // FXC always reports where it saved the object; that is not a diagnostic.
    if (const size_t pos = output.find("compilation object save succeeded"); pos != std::string::npos) {
        output = trim(output.substr(0, pos));
    }
    if (r->exitCode != 0) {
        return std::unexpected(std::string(name) + " failed (exit " + std::to_string(r->exitCode) + ") for " +
                               toString(req_.target) + " " + req_.entry + ":\n" + output);
    }
    if (!output.empty()) {
        diagnostics_.push_back(std::string(name) + ": " + output);
    }
    return {};
}

std::expected<std::vector<uint8_t>, std::string> Job::dxc(const std::vector<Arg>& extra, const std::string& outName,
                                                         bool entryAndProfile) {
    auto exe = requireTool(tools_.dxc, "dxc");
    if (!exe) {
        return std::unexpected(exe.error());
    }
    std::vector<Arg> args;
    if (entryAndProfile) {
        args.insert(args.end(), {"-E", req_.entry, "-T", profile(req_.stage, "6_0")});
    }
    args.insert(args.end(), {"-HV", "2021"});
    for (const auto& [k, v] : req_.defines) {
        args.emplace_back("-D");
        args.emplace_back(v.empty() ? k : k + "=" + v);
    }
    for (const fs::path& inc : req_.includeDirs) {
        args.emplace_back("-I");
        args.emplace_back(fs::absolute(inc));
    }
    args.insert(args.end(), extra.begin(), extra.end());
    const fs::path out = tmp_.path() / outName;
    args.emplace_back(out);
    args.emplace_back(fs::absolute(req_.src));
    if (auto r = tool(*exe, "dxc", args); !r) {
        return std::unexpected(r.error());
    }
    return readFile(out);
}

std::expected<std::vector<uint8_t>, std::string> Job::fxc(const fs::path& hlsl, const std::string& entry) {
    auto exe = requireTool(tools_.fxc, "fxc");
    if (!exe) {
        return std::unexpected(exe.error());
    }
    const fs::path out = tmp_.path() / "out.dxbc";
    if (auto r = tool(*exe, "fxc", {"/nologo", "/T", profile(req_.stage, "5_0"), "/E", entry, "/Fo", out, hlsl}); !r) {
        return std::unexpected(r.error());
    }
    return readFile(out);
}

std::expected<void, std::string> Job::toSpirv(CompileResult& out) {
    out.bytecode = spirvBytes_;
    for (Resource& r : refl_.resources) {
        r.targetBinding = {{"set", r.space}, {"binding", r.index}};
    }
    return {};
}

std::expected<void, std::string> Job::toDxil(CompileResult& out) {
    auto dxil = dxc({"-Fo"}, "out.dxil");
    if (!dxil) {
        return std::unexpected(dxil.error());
    }
    out.bytecode = std::move(*dxil);
    for (Resource& r : refl_.resources) {
        r.targetBinding = {{"register", std::string(1, registerClass(r.kind)) + std::to_string(r.index)}};
    }
    return {};
}

std::expected<void, std::string> Job::toWgsl(CompileResult& out) {
    auto exe = requireTool(tools_.tint, "tint");
    if (!exe) {
        return std::unexpected(exe.error());
    }
    const fs::path in = tmp_.path() / "in.spv";
    const fs::path wgsl = tmp_.path() / "out.wgsl";
    if (auto w = writeFile(in, spirvBytes_.data(), spirvBytes_.size()); !w) {
        return std::unexpected(w.error());
    }
    if (auto r = tool(*exe, "tint", {in, "-o", wgsl}); !r) {
        return std::unexpected(r.error());
    }
    auto text = readFile(wgsl);
    if (!text) {
        return std::unexpected(text.error());
    }
    out.text.assign(text->begin(), text->end());
    for (Resource& r : refl_.resources) {
        r.targetBinding = {{"set", r.space}, {"binding", r.index}};
    }
    return {};
}

std::expected<void, std::string> Job::toMsl(CompileResult& out) {
    spirv_cross::CompilerMSL msl(spirv_);
    auto opts = msl.get_msl_options();
    opts.set_msl_version(3, 0); // Metal 3 (spec §2.1)
    opts.platform = spirv_cross::CompilerMSL::Options::macOS;
    msl.set_msl_options(opts);
    // Explicit, recorded indices: per MSL argument kind, resources in (space, index) order.
    uint32_t buffer = 0, texture = 0, sampler = 0;
    for (Resource& r : refl_.resources) {
        spirv_cross::MSLResourceBinding b;
        b.stage = executionModel(req_.stage);
        b.desc_set = r.space;
        b.binding = r.index;
        b.count = std::max<uint32_t>(r.arraySize, 1);
        uint32_t index = 0;
        if (r.kind == "cbuffer" || r.kind == "storage_ro" || r.kind == "storage_rw") {
            // RWTexture (storage image) is a texture argument in MSL; buffers otherwise.
            const bool image = msl.get_type_from_variable(r.spirvId).basetype == spirv_cross::SPIRType::Image;
            if (image) {
                index = b.msl_texture = texture;
                texture += b.count;
            } else {
                index = b.msl_buffer = buffer;
                buffer += b.count;
            }
        } else if (r.kind == "texture") {
            index = b.msl_texture = texture;
            texture += b.count;
        } else {
            index = b.msl_sampler = sampler;
            sampler += b.count;
        }
        msl.add_msl_resource_binding(b);
        r.targetBinding = {{"mslIndex", index}};
    }
    out.text = msl.compile();
    return {};
}

std::expected<void, std::string> Job::toGlsl(CompileResult& out, uint32_t version) {
    spirv_cross::CompilerGLSL glsl(spirv_);
    auto opts = glsl.get_common_options();
    opts.es = true;
    opts.version = version;
    opts.vertex.fixup_clipspace = true; // D3D/Vulkan clip z [0,1] -> GL [-1,1]
    opts.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
    opts.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
    glsl.set_common_options(opts);

    // Inter-stage variables: GLSL ES 3.00 links VS outputs to FS inputs by NAME (no locations), so both
    // sides get the same location-derived name.
    const spirv_cross::ShaderResources res = glsl.get_shader_resources();
    if (req_.stage == Stage::Vertex) {
        for (const auto& v : res.stage_outputs) {
            if (glsl.has_decoration(v.id, spv::DecorationLocation)) {
                glsl.set_name(v.id, "vary_loc" + std::to_string(glsl.get_decoration(v.id, spv::DecorationLocation)));
            }
        }
    } else if (req_.stage == Stage::Fragment) {
        for (const auto& v : res.stage_inputs) {
            if (glsl.has_decoration(v.id, spv::DecorationLocation)) {
                glsl.set_name(v.id, "vary_loc" + std::to_string(glsl.get_decoration(v.id, spv::DecorationLocation)));
            }
        }
    }

    const bool explicitBindings = version >= 310;
    // Ranges were checked against kGlslBindingsPerSpace in checkTargetFeatures().
    auto flatBinding = [](const Resource& r) { return r.space * kGlslBindingsPerSpace + r.index; };

    glsl.build_combined_image_samplers();
    std::map<uint32_t, std::string> combinedName; // texture/sampler variable id -> first combined name
    auto combos = glsl.get_combined_image_samplers();
    std::sort(combos.begin(), combos.end(), [&](const auto& a, const auto& b) {
        return std::pair(glsl.get_name(a.image_id), glsl.get_name(a.sampler_id)) <
               std::pair(glsl.get_name(b.image_id), glsl.get_name(b.sampler_id));
    });
    for (const auto& c : combos) {
        const std::string name = glsl.get_name(c.image_id) + "_" + glsl.get_name(c.sampler_id);
        glsl.set_name(c.combined_id, name);
        combinedName.emplace(c.image_id, name);
        combinedName.emplace(c.sampler_id, name);
        if (explicitBindings) {
            const auto it = std::find_if(refl_.resources.begin(), refl_.resources.end(),
                                         [&](const Resource& r) { return r.spirvId == c.image_id; });
            if (it != refl_.resources.end()) {
                glsl.set_decoration(c.combined_id, spv::DecorationBinding, flatBinding(*it));
            }
        }
    }
    for (Resource& r : refl_.resources) {
        std::string name;
        const bool block = r.kind == "cbuffer" || r.kind == "storage_ro" || r.kind == "storage_rw";
        if (block) {
            // Block name = what glGetUniformBlockIndex / glGetProgramResourceIndex take (DXC's
            // "type.<name>" would be emitted sanitized, so name it explicitly).
            name = "type_" + r.name;
            glsl.set_name(glsl.get_type_from_variable(r.spirvId).self, name);
            if (explicitBindings) {
                glsl.set_decoration(r.spirvId, spv::DecorationBinding, flatBinding(r));
            }
        } else if (const auto it = combinedName.find(r.spirvId); it != combinedName.end()) {
            name = it->second; // combined sampler<->texture uniform
        }
        r.targetBinding = {{"glslName", name}};
        // Samplers have no GLSL object of their own: the combined sampler takes the texture's binding.
        if (explicitBindings && r.kind != "sampler") {
            r.targetBinding["glslBinding"] = flatBinding(r);
        }
    }
    out.text = glsl.compile();
    return {};
}

std::expected<void, std::string> Job::toDxbcCross(CompileResult& out) {
    spirv_cross::CompilerHLSL hlsl(spirv_);
    auto opts = hlsl.get_hlsl_options();
    opts.shader_model = 50;
    hlsl.set_hlsl_options(opts);
    const auto flat = d3d11FlatRegisters(refl_);
    for (Resource& r : refl_.resources) {
        const char cls = registerClass(r.kind);
        const uint32_t slot = flat.at({cls, r.space, r.index});
        hlsl.set_decoration(r.spirvId, spv::DecorationBinding, slot);
        hlsl.set_decoration(r.spirvId, spv::DecorationDescriptorSet, 0);
        r.targetBinding = {{"register", std::string(1, cls) + std::to_string(slot)}};
    }
    // Keep the HLSL vertex semantics so the D3D11 input layout matches the direct path.
    for (const VertexInput& v : refl_.vertexInputs) {
        if (!v.semantic.empty()) {
            hlsl.add_vertex_attribute_remap({v.location, v.semantic});
        }
    }
    out.text = hlsl.compile();
    const fs::path src = tmp_.path() / "cross.hlsl";
    if (auto w = writeFile(src, out.text); !w) {
        return std::unexpected(w.error());
    }
    auto bc = fxc(src, "main");
    if (!bc) {
        return std::unexpected(bc.error());
    }
    out.bytecode = std::move(*bc);
    return {};
}

std::expected<void, std::string> Job::toDxbcDirect(CompileResult& out) {
    // SM5 has no register spaces: take the DXC-preprocessed source (includes/defines resolved), then
    // rewrite every register(<c><i>, space<S>) to the flattened D3D11 slot.
    const std::string& text = preprocessed_;
    static const std::regex reg(R"(register\s*\(\s*([bBtTsSuU])\s*(\d+)\s*(?:,\s*space\s*(\d+)\s*)?\))");
    const auto flat = d3d11FlatRegisters(refl_, declaredRegisters(text));
    std::string rewritten;
    size_t last = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), reg); it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        const RegisterKey key(static_cast<char>(std::tolower(static_cast<unsigned char>(m[1].str()[0]))),
                              m[3].matched ? static_cast<uint32_t>(std::stoul(m[3].str())) : 0u,
                              static_cast<uint32_t>(std::stoul(m[2].str())));
        rewritten.append(text, last, static_cast<size_t>(m.position(0)) - last);
        rewritten += "register(" + std::string(1, std::get<0>(key)) + std::to_string(flat.at(key)) + ")";
        last = static_cast<size_t>(m.position(0) + m.length(0));
    }
    rewritten.append(text, last, std::string::npos);
    out.text = rewritten;
    const fs::path src = tmp_.path() / "direct.hlsl";
    if (auto w = writeFile(src, out.text); !w) {
        return std::unexpected(w.error());
    }
    auto bc = fxc(src, req_.entry);
    if (!bc) {
        return std::unexpected(bc.error());
    }
    out.bytecode = std::move(*bc);
    for (Resource& r : refl_.resources) {
        const char cls = registerClass(r.kind);
        r.targetBinding = {{"register", std::string(1, cls) + std::to_string(flat.at({cls, r.space, r.index}))}};
    }
    return {};
}

// Features a target cannot express are rejected here, by name, instead of emitting a shader that a
// validator or the driver rejects later (Review Focus 2):
//   GLSL ES 3.00 (T0: GLES 3.0 / WebGL2): no storage buffers, no storage images, no compute.
//   GLSL ES (both): every binding range inside [0, 16) of its space (flattened space * 16 + index).
//   WGSL (WebGPU): no read-write storage in the vertex stage; no resource arrays (binding_array is
//   not core WGSL).
std::expected<void, std::string> Job::checkTargetFeatures() const {
    std::string tier = "TIER undefined";
    for (const auto& [k, v] : req_.defines) {
        if (k == "TIER") {
            tier = "TIER=" + v; // last definition wins, as on the DXC command line
        }
    }
    auto unsupported = [&](const std::string& feature) {
        return std::unexpected("target " + toString(req_.target) + " does not support " + feature + " (" + tier + ")");
    };
    if (req_.target == Target::GLSL_ES300) {
        if (req_.stage == Stage::Compute) {
            return unsupported("compute shaders");
        }
        for (const Resource& r : refl_.resources) {
            if (r.kind == "storage_rw" && r.image) {
                return unsupported("storage images");
            }
            if (r.kind == "storage_ro" || r.kind == "storage_rw") {
                return unsupported("storage buffers");
            }
        }
    }
    if (req_.target == Target::GLSL_ES300 || req_.target == Target::GLSL_ES320) {
        // GLSL ES flattens (space, index) to space * 16 + index: the whole range [index, index + n) of a
        // resource array must stay below 16, and an unbounded array has no range to flatten.
        for (const Resource& r : refl_.resources) {
            const std::string cls(1, registerClass(r.kind));
            if (r.arraySize == 0) {
                return std::unexpected("binding range " + cls + std::to_string(r.index) + "..(unbounded) exceeds " +
                                       std::to_string(kGlslBindingsPerSpace) + " per space for " +
                                       toString(req_.target));
            }
            if (r.index + r.arraySize > kGlslBindingsPerSpace) {
                return std::unexpected("binding range " + cls + std::to_string(r.index) + ".." + cls +
                                       std::to_string(r.index + r.arraySize - 1) + " exceeds " +
                                       std::to_string(kGlslBindingsPerSpace) + " per space for " +
                                       toString(req_.target));
            }
        }
    }
    if (req_.target == Target::WGSL) {
        for (const Resource& r : refl_.resources) {
            if (req_.stage == Stage::Vertex && r.kind == "storage_rw") {
                return unsupported("read-write storage in the vertex stage");
            }
            if (r.arraySize != 1) {
                return unsupported("resource arrays");
            }
        }
    }
    return {};
}

std::expected<CompileResult, std::string> Job::run() {
    std::error_code ec;
    if (!fs::is_regular_file(req_.src, ec)) {
        return std::unexpected("source not found: " + toUtf8(req_.src));
    }
    // Preprocessed source: the declared registers for the binding-convention check (all of them, also
    // the ones other entries use) and the input of the direct DXBC path.
    auto pre = dxc({"-P", "-Fi"}, "pre.hlsl", false);
    if (!pre) {
        return std::unexpected(pre.error());
    }
    preprocessed_.assign(pre->begin(), pre->end());
    // SPIR-V first: it is the reflection source for every target and the input of the cross paths.
    // DXBC cross, pixel shader: keep every declared input (-fspv-preserve-interface). Otherwise DXC drops
    // the inputs the entry does not read, FXC packs the remaining ones from register 0 and the D3D11
    // VS -> PS linkage breaks (debug layer: "Semantic 'TEXCOORD' is defined for mismatched hardware
    // registers between the output stage and input stage"; plan F1 Task 5). Inputs only: no effect on
    // resources/reflection, and the vertex stage keeps its exact input layout.
    std::vector<Arg> spvArgs = {"-spirv", "-fspv-target-env=vulkan1.1"};
    if (req_.target == Target::DXBC && req_.dxbcPath == DxbcPath::Cross && req_.stage == Stage::Fragment) {
        spvArgs.emplace_back("-fspv-preserve-interface");
    }
    spvArgs.emplace_back("-Fo");
    auto spv = dxc(spvArgs, "out.spv");
    if (!spv) {
        return std::unexpected(spv.error());
    }
    spirvBytes_ = std::move(*spv);
    spirv_ = toWords(spirvBytes_);
    std::vector<uint32_t> semantics;
    if (req_.stage == Stage::Vertex) {
        auto sem = dxc({"-spirv", "-fspv-target-env=vulkan1.1", "-fspv-reflect", "-Fo"}, "reflect.spv");
        if (!sem) {
            return std::unexpected(sem.error());
        }
        semantics = toWords(*sem);
    }
    auto refl = reflectSpirv(spirv_, req_.stage, semantics.empty() ? nullptr : &semantics);
    if (!refl) {
        return std::unexpected(refl.error());
    }
    refl_ = std::move(*refl);
    if (auto e = checkExplicitRegisters(refl_, preprocessed_); !e) {
        return std::unexpected(e.error());
    }
    if (auto c = checkBindingConvention(refl_, declaredRegisters(preprocessed_)); !c) {
        return std::unexpected(c.error());
    }
    if (auto f = checkTargetFeatures(); !f) {
        return std::unexpected(f.error());
    }

    CompileResult out;
    std::expected<void, std::string> r;
    try {
        switch (req_.target) {
        case Target::SPIRV: r = toSpirv(out); break;
        case Target::DXIL: r = toDxil(out); break;
        case Target::WGSL: r = toWgsl(out); break;
        case Target::MSL: r = toMsl(out); break;
        case Target::GLSL_ES300: r = toGlsl(out, 300); break;
        case Target::GLSL_ES320: r = toGlsl(out, 320); break;
        case Target::DXBC: r = req_.dxbcPath == DxbcPath::Direct ? toDxbcDirect(out) : toDxbcCross(out); break;
        }
    } catch (const std::exception& e) {
        return std::unexpected("SPIRV-Cross failed for " + toString(req_.target) + " " + req_.entry + ": " + e.what());
    }
    if (!r) {
        return std::unexpected(r.error());
    }
    out.reflection = toJson(refl_, req_.stage, req_.entry, req_.target);
    if (req_.target == Target::DXBC) {
        // The D3D11 RHI rejects a VS / PS pair from different paths (F1 Risk 1; plan F2 Task 4).
        out.reflection["dxbcPath"] = toString(req_.dxbcPath);
    }
    out.diagnostics = std::move(diagnostics_);
    return out;
}

} // namespace

std::expected<CompileResult, std::string> compile(const CompileRequest& req, const ToolPaths& tools) {
    Job job(req, tools);
    return job.run();
}

std::expected<std::vector<fs::path>, std::string> writeOutputs(const CompileResult& r, const fs::path& outDir,
                                                              const fs::path& src, const std::string& entry, Target target) {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        return std::unexpected("cannot create '" + toUtf8(outDir) + "': " + ec.message());
    }
    const std::string base = toUtf8(src.stem()) + "." + entry + "." + toString(target);
    const fs::path main = outDir / fromUtf8(base + "." + extension(target));
    const fs::path refl = outDir / fromUtf8(base + ".refl.json");
    std::expected<void, std::string> w = isTextTarget(target) ? writeFile(main, r.text)
                                                              : writeFile(main, r.bytecode.data(), r.bytecode.size());
    if (!w) {
        return std::unexpected(w.error());
    }
    if (auto wr = writeFile(refl, r.reflection.dump(2) + "\n"); !wr) {
        return std::unexpected(wr.error());
    }
    return std::vector<fs::path>{main, refl};
}

} // namespace eng::sc
