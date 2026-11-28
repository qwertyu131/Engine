// Punctual lights and shadows through the facade (plan F3 Task 7). Same binary / backend configurations as
// test_facade.cpp (renderer_unit_tests --backend=vulkan|d3d11|d3d11-fl10_0).
//
// Scenes: tests/assets/f03/lights/{directional,spot,point}.gltf (scripts/make_f03_assets.py): a wall (z = 0, facing
// the fit camera) and a box in front of it; one light from -x / +z puts the box's shadow on the wall around
// (0.75, 0, 0) while the mirror point (-0.75, 0, 0) is lit. Probes are projected with the tests' own camera model.
// Shadowed = R+G+B at the shadow point <= 0.6 x R+G+B at the lit point (plan: >= 40 % darker); "no shadow" = the two
// points within 15 % of each other. r.IBL.Intensity 0.3 so the environment does not hide the shadow.
//
// Cvars changed here are set at CVarSource::Scalability (the sg.* table's own source) and restored, so the
// process-wide registry stays usable by the other cases (a Code / CommandLine value would outrank sg.* afterwards).
#include "FacadeTestUtil.h"

using namespace facade_test;

namespace {

std::filesystem::path lightsScene(const char* kind) {
    return u8(ENG_TEST_ASSET_DIR) / "f03" / "lights" / (std::string(kind) + ".gltf");
}

// Sets `name` at Scalability for the lifetime of the object, then restores the previous value at Scalability.
struct ScopedCVar {
    eng::Settings& s;
    std::string name, previous;
    ScopedCVar(eng::Settings& settings, std::string n, const std::string& value) : s(settings), name(std::move(n)) {
        eng::CVar* c = s.find(name);
        REQUIRE_MESSAGE(c != nullptr, name);
        const eng::CVarValue& v = c->value();
        if (const bool* b = std::get_if<bool>(&v)) {
            previous = *b ? "1" : "0";
        } else if (const int32_t* i = std::get_if<int32_t>(&v)) {
            previous = std::to_string(*i);
        } else if (const float* f = std::get_if<float>(&v)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.9g", *f);
            previous = buf;
        } else {
            previous = std::get<std::string>(v);
        }
        REQUIRE_MESSAGE(s.set(name, value, eng::CVarSource::Scalability), name << "=" << value);
    }
    ~ScopedCVar() { (void)s.set(name, previous, eng::CVarSource::Scalability); }
};

struct Probes {
    std::array<int, 3> shadow, lit;
    double ratio() const {
        const double l = lit[0] + lit[1] + lit[2];
        return l > 0.0 ? (shadow[0] + shadow[1] + shadow[2]) / l : 1.0;
    }
};

Probes probe(const Fixture& f, const eng::Image& img) {
    const M4 vp = viewProj(f.camera, 1.0);
    const auto s = project(vp, {0.75, 0.0, 0.0}), l = project(vp, {-0.75, 0.0, 0.0});
    return {pixel(img, s[0], s[1]), pixel(img, l[0], l[1])};
}

std::string str(const std::array<int, 3>& p) {
    return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]);
}

} // namespace

TEST_SUITE("renderer.lights") {

TEST_CASE("each punctual light type shadows the wall in normal mode (directional CSM, spot, point cube)") {
    for (const char* kind : {"directional", "spot", "point"}) {
        const std::string kindName = kind;
        CAPTURE(kindName);
        Fixture f = open(lightsScene(kind));
        ScopedCVar ibl(f.r->settings(), "r.IBL.Intensity", "0.3");
        const eng::Image img = render(f);
        const Probes p = probe(f, img);
        MESSAGE(kindName << ": shadow " << str(p.shadow) << " lit " << str(p.lit) << " ratio " << p.ratio());
        CHECK_FALSE(isBackground(p.shadow));
        CHECK_FALSE(isBackground(p.lit));
        CHECK(p.ratio() <= 0.6);
        done(f);
    }
}

TEST_CASE("the facade lists the asset's lights, toggles them and adds one") {
    Fixture f = open(lightsScene("point"));
    ScopedCVar ibl(f.r->settings(), "r.IBL.Intensity", "0.3");
    std::vector<eng::LightInfo> lights = f.r->lights(f.scene);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].name == "bulb");
    CHECK(lights[0].type == eng::LightType::Point);
    CHECK(lights[0].enabled);
    CHECK(lights[0].fromAsset);
    CHECK(std::abs(lights[0].position[0] + 1.2f) < 1e-5f);
    CHECK(std::abs(lights[0].position[2] - 3.0f) < 1e-5f);
    CHECK(lights[0].intensity == 40.0f);
    const Probes on = probe(f, render(f));

    REQUIRE(f.r->setLightEnabled(f.scene, 0, false));
    CHECK_FALSE(f.r->lights(f.scene)[0].enabled);
    CHECK_FALSE(f.r->setLightEnabled(f.scene, 1, false)); // no such light
    const Probes off = probe(f, render(f));
    MESSAGE("on: lit " << str(on.lit) << " / off: lit " << str(off.lit) << " shadow " << str(off.shadow));
    CHECK(off.lit[0] + off.lit[1] + off.lit[2] < on.lit[0] + on.lit[1] + on.lit[2]); // the bulb lit it
    CHECK(off.ratio() > 0.85);                                                     // no light, no shadow

    // An added directional light (world space, pointing down -Z towards the wall) lights both points the same.
    eng::LightInfo sun;
    sun.name = "added sun";
    sun.type = eng::LightType::Directional;
    sun.intensity = 3.0f;
    sun.direction = {0.0f, 0.0f, -1.0f};
    auto idx = f.r->addLight(f.scene, sun);
    REQUIRE_MESSAGE(idx.has_value(), (idx ? std::string() : idx.error().message));
    CHECK(*idx == 1);
    lights = f.r->lights(f.scene);
    REQUIRE(lights.size() == 2);
    CHECK_FALSE(lights[1].fromAsset);
    CHECK(lights[1].enabled);
    const Probes added = probe(f, render(f));
    MESSAGE("added sun: lit " << str(added.lit) << " shadow " << str(added.shadow));
    CHECK(added.lit[0] + added.lit[1] + added.lit[2] > off.lit[0] + off.lit[1] + off.lit[2] + 30);
    CHECK(added.ratio() > 0.85); // straight-on light: the box's shadow is behind the box, not at the probes
    done(f);
}

TEST_CASE("sg.ShadowQuality 0 and 3 render differently; 0 has no shadow") {
    Fixture f = open(lightsScene("directional"));
    ScopedCVar ibl(f.r->settings(), "r.IBL.Intensity", "0.3");
    eng::Image q3, q0;
    {
        ScopedCVar sg(f.r->settings(), "sg.ShadowQuality", "3");
        q3 = render(f);
    }
    {
        ScopedCVar sg(f.r->settings(), "sg.ShadowQuality", "0");
        CHECK_FALSE(f.r->settings().find("r.Shadow.Enable")->getBool());
        q0 = render(f);
    }
    size_t differing = 0;
    for (size_t i = 0; i < q3.rgba8.size(); ++i) {
        differing += q3.rgba8[i] != q0.rgba8[i] ? 1 : 0;
    }
    CHECK(differing > 100);
    const Probes p3 = probe(f, q3), p0 = probe(f, q0);
    MESSAGE("sg 3: ratio " << p3.ratio() << ", sg 0: ratio " << p0.ratio());
    CHECK(p3.ratio() <= 0.6);
    CHECK(p0.ratio() > 0.85);
    done(f);
}

TEST_CASE("r.Shadow.PointLights=0 removes the point light's shadow, not the light") {
    Fixture f = open(lightsScene("point"));
    ScopedCVar ibl(f.r->settings(), "r.IBL.Intensity", "0.3");
    const Probes with = probe(f, render(f));
    Probes without;
    {
        ScopedCVar pl(f.r->settings(), "r.Shadow.PointLights", "0");
        without = probe(f, render(f));
    }
    MESSAGE("point shadows on: ratio " << with.ratio() << ", off: ratio " << without.ratio());
    CHECK(with.ratio() <= 0.6);
    CHECK(without.ratio() > 0.85);
    CHECK(std::abs((without.lit[0] + without.lit[1] + without.lit[2]) - (with.lit[0] + with.lit[1] + with.lit[2])) <=
          6); // the lit point keeps the light
    done(f);
}

TEST_CASE("r.Lighting.MaxLights=0 and r.Lighting.Punctual=0 drop the punctual light; r.Lighting.Specular=0 dims it") {
    Fixture f = open(lightsScene("spot"));
    ScopedCVar ibl(f.r->settings(), "r.IBL.Intensity", "0.3");
    const Probes on = probe(f, render(f));
    Probes none, noPunctual, noSpecular;
    {
        ScopedCVar m(f.r->settings(), "r.Lighting.MaxLights", "0");
        none = probe(f, render(f));
    }
    {
        ScopedCVar m(f.r->settings(), "r.Lighting.Punctual", "0");
        noPunctual = probe(f, render(f));
    }
    {
        ScopedCVar m(f.r->settings(), "r.Lighting.Specular", "0");
        noSpecular = probe(f, render(f));
    }
    const auto sum = [](const std::array<int, 3>& p) { return p[0] + p[1] + p[2]; };
    MESSAGE("lit: on " << str(on.lit) << ", MaxLights 0 " << str(none.lit) << ", Punctual 0 " << str(noPunctual.lit)
                       << ", Specular 0 " << str(noSpecular.lit));
    CHECK(sum(none.lit) + 30 < sum(on.lit));
    CHECK(sum(noPunctual.lit) == sum(none.lit));
    CHECK(sum(noSpecular.lit) <= sum(on.lit));
    CHECK(sum(noSpecular.lit) > sum(none.lit) + 30);
    done(f);
}

TEST_CASE("reference mode draws punctual lights only with r.ReferenceMode.Punctual=1, and never shadows") {
    Fixture f = open(lightsScene("spot"));
    Probes off, on;
    {
        ScopedCVar rm(f.r->settings(), "r.ReferenceMode", "1");
        off = probe(f, render(f));
        {
            ScopedCVar rp(f.r->settings(), "r.ReferenceMode.Punctual", "1");
            on = probe(f, render(f));
        }
    }
    MESSAGE("reference mode: punctual off lit " << str(off.lit) << " shadow " << str(off.shadow) << ", on lit "
                                                << str(on.lit) << " shadow " << str(on.shadow));
    CHECK(off.lit[0] + off.lit[1] + off.lit[2] + 30 < on.lit[0] + on.lit[1] + on.lit[2]);
    CHECK(on.ratio() > 0.6); // the light reaches the shadow point too: no shadow in reference mode
    done(f);
}

} // TEST_SUITE
