// viewer: windowed renderer with an RHI-drawn ImGui (plan F2 Task 6; spec K-G).
//
//   viewer --backend vulkan|d3d11 [--warp] [--adapter <index>] [--scene builtin:...] [--cvar n=v]...
//          [--frames N] [--screenshot <png>] [--script resize] [--size WxH] [--shader-dir <dir>]
//          [--asset-dir <dir>]
//
//   --frames N       renders N frames and exits 0 (fixed 1/60 s animation step, so runs are repeatable);
//                    without it the viewer runs until the window is closed (real-time animation).
//   --screenshot     saves the last frame (read back from the swapchain back buffer before present).
//   --script resize  drives the window over 60 frames: 800x600 -> 1x1 -> minimise -> restore -> 1280x720
//                    (12 frames each) and logs the client size the window really got at every step.
//   F12              saves Saved/Screenshots/<backend>-<yyyymmdd-hhmmss>.png (relative to the working dir).
// CVars: r.VSync (default 1), r.Viewer.ShowUI (default 1); --cvar uses CVarSource::CommandLine, the CVar
// panel CVarSource::Console. Default scene: builtin:textured-cube spinning at 45 deg/s.
//
// Exit codes (plan F2 Global Constraints): 0 ok, 2 usage / IO, 3 backend not available, 4 validation error,
// 5 device lost / GPU error. On exit it prints the frame count, the average fps of the rendered frames and
// the validation message count. All path arguments are UTF-8 (taken from the wide command line).
#include "Panels.h"

#include "Image.h"
#include "Scenes.h"
#include "Utf8.h"
#include "core/CVar.h"
#include "core/Log.h"
#include "platform/Window.h"
#include "rhi/Rhi.h"
#include "rhi/ShaderLibrary.h"
#include "ui/ImGuiRenderer.h"

#include <imgui.h>

#include <chrono>
#include <charconv>
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
    std::string scene{eng::scenes::kTexturedCube};
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
          "         [--asset-dir <dir>]\n"
          "scenes: builtin:triangle, builtin:textured-cube (default), builtin:pbr-spheres\n"
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
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& k = args[i];
        if (k == "--warp") {
            o.warp = true;
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

bool applyCVars(const std::vector<std::string>& cvars) {
    auto& reg = eng::CVarRegistry::get();
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
    for (const eng::rhi::Backend b : eng::rhi::compiledBackends()) {
        if (opts->backend == eng::rhi::toString(b)) {
            backend = b;
        }
    }
    if (!backend) {
        std::cerr << "viewer: backend '" << opts->backend << "' not available in this build\n";
        return kExitNoBackend;
    }
    bool knownScene = false;
    for (const std::string_view n : eng::scenes::sceneNames()) {
        knownScene = knownScene || opts->scene == n;
    }
    if (!knownScene) {
        std::cerr << "viewer: unknown scene '" << opts->scene << "'\n";
        return kExitError;
    }

    auto& reg = eng::CVarRegistry::get();
    eng::CVar* vsyncVar = reg.registerInt("r.VSync", 1, "Present with vertical sync (1) or as fast as possible (0)",
                                          eng::CVarFlags::None, 0, 1);
    eng::CVar* showUi = reg.registerBool("r.Viewer.ShowUI", true, "Draw the viewer's ImGui panels");
    if (!applyCVars(opts->cvars)) {
        return kExitError;
    }

#if defined(ENG_DEFAULT_SHADER_DIR)
    const fs::path shaderDir = eng::rhi::pathFromUtf8(opts->shaderDir.empty() ? ENG_DEFAULT_SHADER_DIR : opts->shaderDir);
#else
    const fs::path shaderDir = eng::rhi::pathFromUtf8(opts->shaderDir);
#endif
#if defined(ENG_DEFAULT_ASSET_DIR)
    const fs::path assetDir = eng::rhi::pathFromUtf8(opts->assetDir.empty() ? ENG_DEFAULT_ASSET_DIR : opts->assetDir);
#else
    const fs::path assetDir = eng::rhi::pathFromUtf8(opts->assetDir);
#endif

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

    eng::rhi::DeviceDesc dd;
    dd.backend = *backend;
    dd.warp = opts->warp;
    dd.adapterIndex = opts->adapter;
    auto devr = eng::rhi::createDevice(dd);
    if (!devr) {
        // "backend not available:" marks the absent-backend exit 3 (the only one ctest skips, plan F3 Task 1).
        std::cerr << "viewer: " << (devr.error().exitCode == kExitNoBackend ? "backend not available: " : "")
                  << devr.error().message << "\n";
        return devr.error().exitCode;
    }
    eng::rhi::IDevice& dev = **devr;
    ENG_LOG_INFO("viewer: backend {}: {} (API {}, tier {})", opts->backend, dev.caps().adapterName,
                 dev.caps().apiVersion, dev.caps().tier);

    const eng::rhi::Format format = eng::rhi::Format::BGRA8_SRGB;
    eng::rhi::SwapchainDesc sd;
    sd.nativeWindow = win.nativeHandle();
    sd.width = win.size().width;
    sd.height = win.size().height;
    sd.format = format;
    sd.vsync = vsyncVar->getInt() != 0;
    auto scr = dev.createSwapchain(sd);
    if (!scr) {
        std::cerr << "viewer: " << scr.error().message << "\n";
        return scr.error().exitCode;
    }
    const eng::rhi::SwapchainHandle swapchain = *scr;
    eng::platform::Window::Size scSize = win.size();
    bool vsync = sd.vsync;

    int exitCode = kExitOk;
    std::string exitMessage;
    {
        eng::scenes::SceneDesc sdesc;
        sdesc.shaderDir = shaderDir;
        sdesc.assetDir = assetDir;
        sdesc.colorFormat = format;
        sdesc.width = std::max(1u, scSize.width);
        sdesc.height = std::max(1u, scSize.height);
        auto scener = eng::scenes::createScene(dev, opts->scene, sdesc);
        if (!scener) {
            std::cerr << "viewer: " << scener.error().message << "\n";
            dev.destroy(swapchain);
            return scener.error().exitCode;
        }
        std::unique_ptr<eng::scenes::Scene> scene = std::move(*scener);
        auto uir = eng::ui::ImGuiRenderer::create(dev, shaderDir, format);
        if (!uir) {
            std::cerr << "viewer: " << uir.error().message << "\n";
            dev.destroy(swapchain);
            return uir.error().exitCode;
        }
        std::unique_ptr<eng::ui::ImGuiRenderer> ui = std::move(*uir);
        uint32_t sceneW = sdesc.width, sceneH = sdesc.height;

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
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            // Swapchain follows the window (a 0 x 0 size is ignored: rendering skips while minimised).
            const eng::platform::Window::Size ws = win.size();
            if (const bool wantVsync = vsyncVar->getInt() != 0; wantVsync != vsync) {
                vsync = wantVsync;
                if (auto r = dev.setSwapchainVsync(swapchain, vsync); !r) {
                    exitCode = r.error().exitCode;
                    exitMessage = r.error().message;
                    break;
                }
            }
            if (win.minimized() || ws.width == 0 || ws.height == 0) {
                // Nothing to draw: sleep until a window message or 16 ms (plan F3 Task 1; was a busy spin).
                win.waitEvents(16);
                continue;
            }
            if (!(ws == scSize)) {
                if (auto r = dev.resizeSwapchain(swapchain, ws.width, ws.height); !r) {
                    exitCode = r.error().exitCode;
                    exitMessage = r.error().message;
                    break;
                }
                scSize = ws;
            }
            if (ws.width != sceneW || ws.height != sceneH) {
                if (auto r = scene->resize(ws.width, ws.height); !r) {
                    exitCode = r.error().exitCode;
                    exitMessage = r.error().message;
                    break;
                }
                sceneW = ws.width;
                sceneH = ws.height;
            }
            const float t = opts->frames ? static_cast<float>(frame) / 60.0f
                                         : std::chrono::duration<float>(now - start).count();
            scene->setTime(t);
            ui->newFrame(dt, ws.width, ws.height, win.dpiScale(), events);
            if (showUi->getBool()) {
                eng::viewer::drawPanels(dev.caps(), stats);
            }

            const auto f0 = std::chrono::steady_clock::now();
            eng::rhi::ICommandList& cmd = dev.beginFrame();
            auto bb = dev.acquireNextImage(swapchain);
            if (!bb) {
                exitCode = bb.error().exitCode;
                exitMessage = bb.error().message;
                (void)dev.present(swapchain);
                break;
            }
            if (!*bb) { // nothing to draw this frame (swapchain being rebuilt / zero size)
                ImGui::EndFrame();
                if (auto r = dev.present(swapchain); !r) {
                    exitCode = r.error().exitCode;
                    exitMessage = r.error().message;
                    break;
                }
                continue;
            }
            const eng::rhi::TextureBarrier toTarget{*bb, eng::rhi::ResourceState::Undefined,
                                                    eng::rhi::ResourceState::RenderTarget};
            cmd.barrier({&toTarget, 1});
            eng::rhi::Result<void> rec = scene->record(cmd, *bb);
            if (rec) {
                rec = ui->render(cmd, *bb, ws.width, ws.height);
            }
            const bool lastFrame = total > 0 && frame == total - 1;
            const bool shoot = (lastFrame && !opts->screenshot.empty()) || f12;
            eng::rhi::Result<void> pres;
            if (rec && shoot) {
                const eng::rhi::TextureBarrier toCopy{*bb, eng::rhi::ResourceState::RenderTarget,
                                                      eng::rhi::ResourceState::CopySrc};
                cmd.barrier({&toCopy, 1});
                pres = dev.submitAndWait();
                if (pres) {
                    auto img = dev.readback(*bb);
                    if (!img) {
                        pres = std::unexpected(img.error());
                    } else {
                        fs::path out = lastFrame && !opts->screenshot.empty()
                                           ? eng::golden::fromUtf8(opts->screenshot)
                                           : fs::path("Saved") / "Screenshots" /
                                                 (opts->backend + "-" + timestamp() + ".png");
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
                        } else {
                            ENG_LOG_INFO("viewer: screenshot {} ({}x{})", eng::golden::toUtf8(out), im.width, im.height);
                            screenshotTaken = screenshotTaken || lastFrame;
                        }
                    }
                }
                f12 = false;
                eng::rhi::ICommandList& cmd2 = dev.beginFrame();
                (void)dev.acquireNextImage(swapchain); // the same, still acquired image
                const eng::rhi::TextureBarrier toPresent{*bb, eng::rhi::ResourceState::CopySrc,
                                                         eng::rhi::ResourceState::Present};
                cmd2.barrier({&toPresent, 1});
                const eng::rhi::Result<void> p2 = dev.present(swapchain);
                if (pres) {
                    pres = p2;
                }
            } else {
                const eng::rhi::TextureBarrier toPresent{*bb, eng::rhi::ResourceState::RenderTarget,
                                                         eng::rhi::ResourceState::Present};
                cmd.barrier({&toPresent, 1});
                pres = dev.present(swapchain);
            }
            if (!rec) {
                exitCode = rec.error().exitCode;
                exitMessage = rec.error().message;
                break;
            }
            if (!pres) {
                exitCode = pres.error().exitCode;
                exitMessage = pres.error().message;
                break;
            }
            if (exitCode != kExitOk) {
                break;
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
                    rendered, wall > 0.0 ? rendered / wall : 0.0, wall, opts->backend.c_str(), scSize.width,
                    scSize.height, vsync ? 1 : 0, rendered ? renderedMs / rendered : 0.0);
        ui.reset();
        scene.reset();
    }
    dev.destroy(swapchain);
    if (auto r = dev.submitAndWait(); !r && exitCode == kExitOk) {
        exitCode = r.error().exitCode;
        exitMessage = r.error().message;
    }
    std::printf("viewer: validation messages: %u\n", dev.validationErrorCount());
    std::fflush(stdout);
    if (exitCode == kExitOk && dev.validationErrorCount() > 0) {
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
