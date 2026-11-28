#pragma once
// Native window (plan F2 Task 6; spec §5.2 platform/). Win32 for now (engine/platform/win32); X11 / Wayland,
// Cocoa / UIKit, Android and the Emscripten canvas come with their phases.
//
// The window owns DPI awareness (per-monitor v2 on Windows): sizes are physical pixels, dpiScale() is
// the monitor scale (1.0 = 96 DPI). Input is queued as platform-neutral events for the UI layer.
#include "rhi/Types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eng::platform {

struct WindowDesc {
    std::string title = "eng"; // UTF-8
    uint32_t width = 1280;     // client area, physical pixels
    uint32_t height = 720;
};

enum class Key : uint8_t {
    None, Tab, Left, Right, Up, Down, PageUp, PageDown, Home, End, Insert, Delete, Backspace, Space, Enter, Escape,
    A, C, V, X, Y, Z, F12, Ctrl, Shift, Alt,
};

struct InputEvent {
    enum class Type : uint8_t { MouseMove, MouseButton, MouseWheel, Key, Char, Focus };
    Type type = Type::MouseMove;
    float x = 0.0f, y = 0.0f; // MouseMove: client pixels; MouseWheel: x = horizontal, y = vertical notches
    int button = 0;           // MouseButton: 0 left, 1 right, 2 middle
    bool down = false;        // MouseButton / Key: pressed; Focus: gained
    Key key = Key::None;
    uint32_t ch = 0;          // Char: Unicode code point
};

class Window {
public:
    struct Size {
        uint32_t width = 0, height = 0;
        friend bool operator==(Size, Size) = default;
    };

    // Error{2, ...} if the window cannot be created.
    static rhi::Result<std::unique_ptr<Window>> create(const WindowDesc& desc);
    virtual ~Window() = default;

    virtual void* nativeHandle() const = 0; // HWND on Windows
    virtual void pollEvents() = 0;          // handles every pending message, never blocks
    // Blocks until a message arrives or timeoutMs passes, then handles every pending message (the viewer's
    // minimised loop, plan F3 Task 1: no busy spin while nothing is drawn).
    virtual void waitEvents(uint32_t timeoutMs) = 0;
    virtual Size size() const = 0;          // client area in physical pixels; 0 x 0 while minimised
    virtual bool minimized() const = 0;
    virtual bool shouldClose() const = 0;   // the user closed the window
    virtual float dpiScale() const = 0;
    virtual std::vector<InputEvent> takeEvents() = 0; // input since the last call

    // Programmatic control (viewer --script resize). The OS may clamp the size; size() tells the result.
    virtual void setClientSize(uint32_t width, uint32_t height) = 0;
    virtual void minimize() = 0;
    virtual void restore() = 0;
};

} // namespace eng::platform
