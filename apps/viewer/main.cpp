// viewer: windowed renderer with an RHI-drawn ImGui (plan F2 Task 6; spec K-G).
//
//   viewer --backend vulkan|d3d11 [--warp] [--adapter <index>] [--scene builtin:...|<file.gltf|.glb>] [--cvar n=v]...
//          [--frames N] [--screenshot <png>] [--script resize] [--size WxH] [--shader-dir <dir>]
//          [--asset-dir <dir>] [<file.gltf|.glb>]
//
//   <file>           (plan F3 Task 8) the asset to show, as --scene. A .gltf / .glb dropped onto the window replaces the
//                    scene (the previous one is released); the viewer logs "viewer: dropped <path>".
//   Camera (glTF scenes): orbit around the fit camera's target. Left mouse drag = rotate, right mouse drag = pan,
//                    wheel = zoom, F = fit (Sample-Viewer resetView). Input over a panel goes to the panel.
//   Panels: Device, Frame time, CVars (left), Asset (extensions used: loaded / ignored with the reason) and Lights
//                    (per-light toggles, sg.ShadowQuality / sg.LightingQuality, r.ReferenceMode[.Punctual]) (right).
//
//   --frames N       renders N frames and exits 0 (fixed 1/60 s animation step, so runs are repeatable);
//                    without it the viewer runs until the window is closed (real-time animation).
//   --screenshot     saves the last frame (read back from the swapchain back buffer before present).
//   --script resize  drives the window over 60 frames: 800x600 -> 1x1 -> minimise -> restore -> 1280x720
//                    (12 frames each) and logs the client size the window really got at every step.
//   F12              saves Saved/Screenshots/<backend>-<yyyymmdd-hhmmss>.png (relative to the working dir).
// CVars: r.VSync (default 1), r.Viewer.ShowUI (default 1); --cvar uses CVarSource::CommandLine, the CVar
// panel CVarSource::Console. Default scene: builtin:textured-cube spinning at 45 deg/s. Everything GPU goes through
// the library facade (include/engine, spec K-D, plan F3 Task 4): window from engine/Window.h, eng::Renderer with the
// window handle and its Dear ImGui overlay (beginUi + ImGui:: calls, drawn by renderFrame).
//
// Exit codes (plan F2 Global Constraints): 0 ok, 2 usage / IO, 3 backend not available, 4 validation error,
// 5 device lost / GPU error. On exit it prints the frame count, the average fps of the rendered frames and
// the validation message count. All path arguments are UTF-8 (taken from the wide command line).
#include "Panels.h"

#include "Image.h"
#include "Utf8.h"

#include <engine/Renderer.h>
#include <engine/Window.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 2;
using eng::rhi::kExitNoBackend;
using eng::rhi::kExitValidation;

struct Options {
    std::string backend;
    bool warp = false;
    int adapter = -1;
    std::string scene{"builtin:textured-cube"};
    std::vector<std::string> cvars;
    std::optional<int> frames;
    std::string screenshot;
    bool scriptResize = false;
    uint32_t width = 1280, height = 720;
    std::string shaderDir, assetDir;
};

void printUsage(std::ostream& os) {
    os << "usage:\n"
          "  viewer --backend vulkan|d3d11 [--warp] [--adapter <index>] [--scene builtin:...] [--cvar n=v]...\n"
          "         [--frames N] [--screenshot <png>] [--script resize] [--size WxH] [--shader-dir <dir>]\n"
          "         [--asset-dir <dir>] [<file.gltf|.glb>]\n"
          "scenes: builtin:triangle, builtin:textured-cube (default), builtin:pbr-spheres, <file>.gltf / .glb\n"
          "exit codes: 0 = ok, 2 = usage / IO error, 3 = backend not available, 4 = validation error,\n"
          "            5 = device lost / GPU error\n";
}

std::optional<int> parseInt(const std::string& s, int min, int max) {
    int v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size() || v < min || v > max) {
        return std::nullopt;
    }
    return v;
}

std::optional<Options> parseArgs(const std::vector<std::string>& args) {
    Options o;
    bool positional = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& k = args[i];
        if (k == "--warp") {
            o.warp = true;
            continue;
        }
        if (k.rfind("--", 0) != 0) {
            if (positional) {
                std::cerr << "viewer: more than one file given ('" << k << "')\n";
                return std::nullopt;
            }
            o.scene = k; // viewer [options] <file.gltf|.glb>
            positional = true;
            continue;
        }
        if (i + 1 >= args.size()) {
            std::cerr << "viewer: option '" << k << "' needs a value (or is unknown)\n";
            return std::nullopt;
        }
        const std::string& v = args[++i];
        if (k == "--backend") {
            o.backend = v;
        } else if (k == "--adapter") {
            auto n = parseInt(v, 0, 64);
            if (!n) {
                std::cerr << "viewer: --adapter must be an integer in [0, 64]\n";
                return std::nullopt;
            }
            o.adapter = *n;
        } else if (k == "--scene") {
            o.scene = v;
        } else if (k == "--cvar") {
            o.cvars.push_back(v);
        } else if (k == "--frames") {
            o.frames = parseInt(v, 1, 10'000'000);
            if (!o.frames) {
                std::cerr << "viewer: --frames must be a positive integer\n";
                return std::nullopt;
            }
        } else if (k == "--screenshot") {
            o.screenshot = v;
        } else if (k == "--script") {
            if (v != "resize") {
                std::cerr << "viewer: unknown --script '" << v << "' (resize)\n";
                return std::nullopt;
            }
            o.scriptResize = true;
        } else if (k == "--size") {
            const size_t x = v.find('x');
            auto w = x == std::string::npos ? std::nullopt : parseInt(v.substr(0, x), 1, 16384);
            auto h = x == std::string::npos ? std::nullopt : parseInt(v.substr(x + 1), 1, 16384);
            if (!w || !h) {
                std::cerr << "viewer: --size expects WxH (got '" << v << "')\n";
                return std::nullopt;
            }
            o.width = static_cast<uint32_t>(*w);
            o.height = static_cast<uint32_t>(*h);
        } else if (k == "--shader-dir") {
            o.shaderDir = v;
        } else if (k == "--asset-dir") {
            o.assetDir = v;
        } else {
            std::cerr << "viewer: unknown option '" << k << "'\n";
            return std::nullopt;
        }
    }
    if (o.backend.empty()) {
        std::cerr << "viewer: --backend is required\n";
        return std::nullopt;
    }
    return o;
}

bool applyCVars(eng::Settings& reg, const std::vector<std::string>& cvars) {
    for (const std::string& kv : cvars) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) {
            std::cerr << "viewer: --cvar expects name=value (got '" << kv << "')\n";
            return false;
        }
        const std::string name = kv.substr(0, eq), value = kv.substr(eq + 1);
        if (!reg.find(name)) {
            std::cerr << "viewer: unknown cvar '" << name << "'\n";
            return false;
        }
        if (!reg.set(name, value, eng::CVarSource::CommandLine)) {
            std::cerr << "viewer: cvar '" << name << "' rejected value '" << value << "'\n";
            return false;
        }
    }
    return true;
}

// Orbit camera around the fit camera's target (plan F3 Task 8): yaw / pitch 0 = the fit camera (looking down -Z).
struct Orbit {
    eng::Camera fit;
    std::array<float, 3> target{0, 0, 0};
    float distance = 1.0f, fitDistance = 1.0f, yaw = 0.0f, pitch = 0.0f;

    void reset(const eng::Camera& c) {
        fit = c;
        target = c.target;
        const float dx = c.position[0] - c.target[0], dy = c.position[1] - c.target[1], dz = c.position[2] - c.target[2];
        distance = fitDistance = std::max(1e-4f, std::sqrt(dx * dx + dy * dy + dz * dz));
        yaw = pitch = 0.0f;
    }
    eng::Camera camera() const {
        eng::Camera c = fit;
        const float cp = std::cos(pitch);
        c.target = target;
        c.position = {target[0] + distance * cp * std::sin(yaw), target[1] + distance * std::sin(pitch),
                      target[2] + distance * cp * std::cos(yaw)};
        c.up = {0.0f, 1.0f, 0.0f};
        c.zfar = std::max(fit.zfar, fit.zfar + (distance - fitDistance));
        return c;
    }
    void rotate(float dx, float dy) {
        yaw -= dx * 0.008f;
        pitch = std::clamp(pitch + dy * 0.008f, -1.55f, 1.55f);
    }
    void pan(float dx, float dy, float viewportHeight) {
        const eng::Camera c = camera();
        std::array<float, 3> f{c.target[0] - c.position[0], c.target[1] - c.position[1], c.target[2] - c.position[2]};
        const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        for (float& v : f) {
            v /= fl;
        }
        std::array<float, 3> r{f[1] * c.up[2] - f[2] * c.up[1], f[2] * c.up[0] - f[0] * c.up[2], f[0] * c.up[1] - f[1] * c.up[0]};
        const float rl = std::max(1e-6f, std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]));
        for (float& v : r) {
            v /= rl;
        }
        const std::array<float, 3> u{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        const float k = distance * 2.0f * std::tan(fit.yfov * 0.5f) / std::max(1.0f, viewportHeight);
        for (int i = 0; i < 3; ++i) {
            target[i] += (-r[i] * dx + u[i] * dy) * k;
        }
    }
    void zoom(float notches) { distance = std::max(1e-4f, distance * std::pow(0.9f, notches)); }
};

bool isGltfPath(const std::string& p) {
    std::string ext = p.size() >= 5 ? p.substr(p.size() - 5) : p;
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return ext == ".gltf" || (ext.size() >= 4 && ext.substr(ext.size() - 4) == ".glb");
}

std::string timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

int run(const std::vector<std::string>& args) {
    if (args.size() == 2 && (args[1] == "--help" || args[1] == "-h")) {
        printUsage(std::cout);
        return kExitOk;
    }
    auto opts = parseArgs(args);
    if (!opts) {
        printUsage(std::cerr);
        return kExitError;
    }
    std::optional<eng::rhi::Backend> backend;
    for (const eng::rhi::Backend b : eng::Renderer::compiledBackends()) {
        if (opts->backend == eng::rhi::toString(b)) {
            backend = b;
        }
    }
    if (!backend) {
        std::cerr << "viewer: backend '" << opts->backend << "' not available in this build\n";
        return kExitNoBackend;
    }
    if (opts->scene.rfind("builtin:", 0) == 0) {
        bool knownScene = false;
        for (const std::string_view n : eng::Renderer::builtinScenes()) {
            knownScene = knownScene || opts->scene == n;
        }
        if (!knownScene) {
            std::cerr << "viewer: unknown scene '" << opts->scene << "'\n";
            return kExitError;
        }
    }

    eng::Settings settings;
    eng::CVar* vsyncVar = settings.registry().registerInt(
        "r.VSync", 1, "Present with vertical sync (1) or as fast as possible (0)", eng::CVarFlags::None, 0, 1);
    eng::CVar* showUi = settings.registry().registerBool("r.Viewer.ShowUI", true, "Draw the viewer's ImGui panels");

    eng::platform::WindowDesc wd;
    wd.title = "eng viewer - " + opts->backend + (opts->warp ? " (WARP)" : "") + " - " + opts->scene;
    wd.width = opts->scriptResize ? 800 : opts->width;
    wd.height = opts->scriptResize ? 600 : opts->height;
    auto window = eng::platform::Window::create(wd);
    if (!window) {
        std::cerr << "viewer: " << window.error().message << "\n";
        return window.error().exitCode;
    }
    eng::platform::Window& win = **window;

    // Everything GPU through the library facade (include/engine, spec K-D, plan F3 Task 4).
    eng::RendererDesc rd;
    rd.backend = *backend;
    rd.warp = opts->warp;
    rd.adapterIndex = opts->adapter;
    rd.window.handle = win.nativeHandle();
    rd.width = win.size().width;
    rd.height = win.size().height;
    rd.debugUi = true;
#if defined(ENG_DEFAULT_SHADER_DIR)
    rd.shaderDir = eng::golden::fromUtf8(opts->shaderDir.empty() ? ENG_DEFAULT_SHADER_DIR : opts->shaderDir);
#else
    rd.shaderDir = eng::golden::fromUtf8(opts->shaderDir);
#endif
#if defined(ENG_DEFAULT_ASSET_DIR)
    rd.assetDir = eng::golden::fromUtf8(opts->assetDir.empty() ? ENG_DEFAULT_ASSET_DIR : opts->assetDir);
#else
    rd.assetDir = eng::golden::fromUtf8(opts->assetDir);
#endif
    // --cvar before the renderer exists only for the viewer's own cvars; the renderer's are registered by create,
    // so the command line is applied (CommandLine source) right after it, before vsync / the scene are read.
    rd.vsync = true;
    auto rr = eng::Renderer::create(rd);
    if (!rr) {
        std::cerr << "viewer: " << (rr.error().exitCode == kExitNoBackend ? "backend not available: " : "")
                  << rr.error().message << "\n";
        return rr.error().exitCode;
    }
    eng::Renderer& renderer = **rr;
    if (!applyCVars(settings, opts->cvars)) {
        return kExitError;
    }
    bool vsync = vsyncVar->getInt() != 0;
    if (!vsync) {
        if (auto r = renderer.setVsync(false); !r) {
            std::cerr << "viewer: " << r.error().message << "\n";
            return r.error().exitCode;
        }
    }
    std::cerr << "[info] viewer: backend " << opts->backend << ": " << renderer.caps().adapterName << " (API "
              << renderer.caps().apiVersion << ", tier " << renderer.caps().tier << ")\n";

    int exitCode = kExitOk;
    std::string exitMessage;
    {
        auto loaded = renderer.loadAsset(eng::golden::fromUtf8(opts->scene));
        if (!loaded) {
            std::cerr << "viewer: " << loaded.error().message << "\n";
            return loaded.error().exitCode;
        }
        auto instantiated = renderer.instantiate(*loaded);
        if (!instantiated) {
            std::cerr << "viewer: " << instantiated.error().message << "\n";
            return instantiated.error().exitCode;
        }
        eng::AssetHandle asset = *loaded;
        eng::SceneHandle scene = *instantiated;
        eng::platform::Window::Size size = win.size();
        const auto aspectOf = [](eng::platform::Window::Size s) {
            return float(std::max(1u, s.width)) / float(std::max(1u, s.height));
        };
        Orbit orbit;
        orbit.reset(renderer.fitCamera(scene, aspectOf(size)));
        renderer.setCamera(orbit.camera());
        bool dragRotate = false, dragPan = false;
        float mouseX = 0.0f, mouseY = 0.0f;

        eng::viewer::FrameStats stats;
        const auto start = std::chrono::steady_clock::now();
        auto last = start;
        double renderedMs = 0.0;
        int rendered = 0;
        bool screenshotTaken = false;
        bool f12 = false;
        const int total = opts->frames.value_or(-1);
        for (int frame = 0; total < 0 || frame < total; ++frame) {
            win.pollEvents();
            if (win.shouldClose()) {
                break;
            }
            if (opts->scriptResize) {
                const char* step = nullptr;
                if (frame == 0) {
                    win.setClientSize(800, 600);
                    step = "client";
                } else if (frame == 12) {
                    win.setClientSize(1, 1);
                    step = "client";
                } else if (frame == 24) {
                    win.minimize();
                    step = "minimised client";
                } else if (frame == 36) {
                    win.restore();
                    step = "restored, client";
                } else if (frame == 48) {
                    win.setClientSize(1280, 720);
                    step = "client";
                }
                if (step) {
                    std::printf("viewer: frame %d script: %s %ux%u\n", frame, step, win.size().width, win.size().height);
                    std::fflush(stdout);
                }
            }
            std::vector<eng::platform::InputEvent> events = win.takeEvents();
            for (const auto& e : events) {
                if (e.type == eng::platform::InputEvent::Type::Key && e.key == eng::platform::Key::F12 && e.down) {
                    f12 = true;
                }
            }
            // Drag-and-drop (plan F3 Task 8): the last dropped .gltf / .glb replaces the scene.
            for (const std::string& path : win.takeDroppedFiles()) {
                if (!isGltfPath(path)) {
                    std::cerr << "[warn] viewer: dropped file is not a .gltf / .glb: " << path << "\n";
                    continue;
                }
                std::printf("viewer: dropped %s\n", path.c_str());
                std::fflush(stdout);
                auto a = renderer.loadAsset(eng::golden::fromUtf8(path));
                if (!a) {
                    std::cerr << "[error] viewer: cannot load the dropped file: " << a.error().message << "\n";
                    continue;
                }
                auto s = renderer.instantiate(*a);
                if (!s) {
                    std::cerr << "[error] viewer: cannot show the dropped file: " << s.error().message << "\n";
                    renderer.releaseAsset(*a);
                    renderer.setActiveScene(scene);
                    continue;
                }
                renderer.releaseAsset(asset);
                renderer.setActiveScene(*s);
                asset = *a;
                scene = *s;
                orbit.reset(renderer.fitCamera(scene, aspectOf(win.size())));
            }
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            const eng::platform::Window::Size ws = win.size();
            if (const bool wantVsync = vsyncVar->getInt() != 0; wantVsync != vsync) {
                vsync = wantVsync;
                if (auto r = renderer.setVsync(vsync); !r) {
                    exitCode = r.error().exitCode;
                    exitMessage = r.error().message;
                    break;
                }
            }
            if (win.minimized() || ws.width == 0 || ws.height == 0) {
                win.waitEvents(16);
                continue;
            }
            if (!(ws == size)) {
                renderer.resize(ws.width, ws.height);
                size = ws;
            }
            const float t = opts->frames ? static_cast<float>(frame) / 60.0f
                                         : std::chrono::duration<float>(now - start).count();
            renderer.setTime(t);
            renderer.beginUi(dt, win.dpiScale(), events);
            // Orbit camera input (not when the UI wants the mouse / keyboard).
            const bool uiMouse = ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse && showUi->getBool();
            const bool uiKeys = ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard && showUi->getBool();
            for (const auto& e : events) {
                using T = eng::platform::InputEvent::Type;
                if (e.type == T::MouseMove) {
                    const float dx = e.x - mouseX, dy = e.y - mouseY;
                    mouseX = e.x;
                    mouseY = e.y;
                    if (dragRotate) {
                        orbit.rotate(dx, dy);
                    } else if (dragPan) {
                        orbit.pan(dx, dy, float(ws.height));
                    }
                } else if (e.type == T::MouseButton) {
                    const bool pressed = e.down && !uiMouse;
                    if (e.button == 0) {
                        dragRotate = pressed;
                    } else if (e.button == 1) {
                        dragPan = pressed;
                    }
                } else if (e.type == T::MouseWheel && !uiMouse) {
                    orbit.zoom(e.y);
                } else if (e.type == T::Key && e.key == eng::platform::Key::F && e.down && !uiKeys) {
                    orbit.reset(renderer.fitCamera(scene, aspectOf(ws)));
                }
            }
            renderer.setCamera(orbit.camera());
            if (showUi->getBool()) {
                eng::viewer::drawPanels(renderer.caps(), stats);
                eng::viewer::drawScenePanels(renderer, asset, scene, float(ws.width));
            }
            const bool lastFrame = total > 0 && frame == total - 1;
            const bool shoot = (lastFrame && !opts->screenshot.empty()) || f12;
            if (shoot) {
                renderer.requestCapture();
            }
            const auto f0 = std::chrono::steady_clock::now();
            if (auto r = renderer.renderFrame(); !r) {
                exitCode = r.error().exitCode;
                exitMessage = r.error().message;
                break;
            }
            if (shoot) {
                f12 = false;
                auto img = renderer.readback();
                if (img) {
                    fs::path out = lastFrame && !opts->screenshot.empty()
                                       ? eng::golden::fromUtf8(opts->screenshot)
                                       : fs::path("Saved") / "Screenshots" / (opts->backend + "-" + timestamp() + ".png");
                    if (out.has_parent_path()) {
                        std::error_code ec;
                        fs::create_directories(out.parent_path(), ec);
                    }
                    eng::golden::Image im;
                    im.width = static_cast<int>(img->width);
                    im.height = static_cast<int>(img->height);
                    im.channels = 4;
                    im.rgba8 = std::move(img->rgba8);
                    if (!eng::golden::savePng(out, im)) {
                        std::cerr << "viewer: cannot write '" << eng::golden::toUtf8(out) << "'\n";
                        exitCode = kExitError;
                        break;
                    }
                    std::cerr << "[info] viewer: screenshot " << eng::golden::toUtf8(out) << " (" << im.width << "x"
                              << im.height << ")\n";
                    screenshotTaken = screenshotTaken || lastFrame;
                } // no captured frame (zero-size swapchain this frame): nothing to save
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
            stats.push(dt * 1000.0f);
            renderedMs += ms;
            if (++rendered == 1) {
                std::printf("viewer: first frame presented\n"); // tests/viewer/run_viewer.py waits for it
                std::fflush(stdout);
            }
        }
        if (exitCode == kExitOk && !opts->screenshot.empty() && !screenshotTaken) {
            exitCode = kExitError;
            exitMessage = "no screenshot taken (the last frame was not rendered)";
        }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("viewer: frames rendered %d, %.1f fps over %.2f s (wall clock, %s, %ux%u, vsync %d), "
                    "frame_ms_avg %.3f (CPU record + submit)\n",
                    rendered, wall > 0.0 ? rendered / wall : 0.0, wall, opts->backend.c_str(), size.width, size.height,
                    vsync ? 1 : 0, rendered ? renderedMs / rendered : 0.0);
    }
    if (auto r = renderer.finish(); !r && exitCode == kExitOk) {
        exitCode = r.error().exitCode;
        exitMessage = r.error().message;
    }
    std::printf("viewer: validation messages: %u\n", renderer.validationErrorCount());
    std::fflush(stdout);
    if (exitCode == kExitOk && renderer.validationErrorCount() > 0) {
        exitCode = kExitValidation;
        exitMessage = "validation messages";
    }
    if (exitCode != kExitOk) {
        std::cerr << "viewer: " << exitMessage << "\n";
    }
    return exitCode;
}

int guardedRun(const std::vector<std::string>& args) {
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::cerr << "viewer: error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "viewer: unknown error\n";
    }
    return kExitError;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    try {
        for (int i = 0; i < argc; ++i) {
            args.push_back(eng::golden::toUtf8(fs::path(argv[i])));
        }
    } catch (const std::exception& e) {
        std::cerr << "viewer: cannot decode command line: " << e.what() << "\n";
        return kExitError;
    }
    return guardedRun(args);
}
