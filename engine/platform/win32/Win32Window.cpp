// Win32 window (plan F2 Task 6): per-monitor v2 DPI awareness, WS_OVERLAPPEDWINDOW, input as InputEvents.
// The resize script (plan F2 Review Focus 3) must really reach a 1 x 1 swapchain: WM_GETMINMAXINFO lowers
// the minimum tracking size, and a size a captioned window cannot take (narrower than its caption
// buttons) switches the window to WS_POPUP (setClientSize).
#include "platform/Window.h"

#include <windows.h>

#include <string>
#include <utility>

namespace eng::platform {

namespace {

constexpr wchar_t kClassName[] = L"eng.Window";

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

Key mapKey(WPARAM vk) {
    switch (vk) {
    case VK_TAB: return Key::Tab;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_INSERT: return Key::Insert;
    case VK_DELETE: return Key::Delete;
    case VK_BACK: return Key::Backspace;
    case VK_SPACE: return Key::Space;
    case VK_RETURN: return Key::Enter;
    case VK_ESCAPE: return Key::Escape;
    case 'A': return Key::A;
    case 'C': return Key::C;
    case 'V': return Key::V;
    case 'X': return Key::X;
    case 'Y': return Key::Y;
    case 'Z': return Key::Z;
    case VK_F12: return Key::F12;
    case VK_CONTROL: return Key::Ctrl;
    case VK_SHIFT: return Key::Shift;
    case VK_MENU: return Key::Alt;
    default: return Key::None;
    }
}

class Win32Window final : public Window {
public:
    ~Win32Window() override {
        if (hwnd_) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
        }
    }

    rhi::Result<void> init(const WindowDesc& desc) {
        // Per-monitor v2: the process sees physical pixels; may fail if already set (e.g. by a manifest).
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Win32Window::wndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return std::unexpected(rhi::Error{rhi::kExitUsage, "window: RegisterClassExW failed"});
        }
        const DWORD style = WS_OVERLAPPEDWINDOW;
        const UINT dpi = GetDpiForSystem();
        RECT r{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
        hwnd_ = CreateWindowExW(0, kClassName, widen(desc.title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, this);
        if (!hwnd_) {
            return std::unexpected(rhi::Error{rhi::kExitUsage, "window: CreateWindowExW failed (" +
                                                                   std::to_string(GetLastError()) + ")"});
        }
        // The window may have opened on a monitor with another DPI: size the client area there.
        setClientSize(desc.width, desc.height);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
        updateSize();
        return {};
    }

    void* nativeHandle() const override { return hwnd_; }

    void pollEvents() override {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        updateSize();
    }

    Size size() const override { return size_; }
    bool minimized() const override { return minimized_; }
    bool shouldClose() const override { return close_; }
    float dpiScale() const override { return static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f; }
    std::vector<InputEvent> takeEvents() override { return std::exchange(events_, {}); }

    void setClientSize(uint32_t width, uint32_t height) override {
        if (IsIconic(hwnd_)) {
            ShowWindow(hwnd_, SW_RESTORE);
        }
        applyClientSize(width, height, kFramedStyle);
        if (size_.width != width || size_.height != height) {
            // A captioned window cannot be narrower than its caption buttons (Windows clamps it, e.g. 120 px):
            // a borderless window can, so tiny sizes (resize script, 1 x 1) switch to WS_POPUP.
            applyClientSize(width, height, WS_POPUP | WS_VISIBLE);
        }
    }
    void minimize() override {
        ShowWindow(hwnd_, SW_MINIMIZE);
        pollEvents();
    }
    void restore() override {
        ShowWindow(hwnd_, SW_RESTORE);
        pollEvents();
    }

private:
    static constexpr DWORD kFramedStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;

    void applyClientSize(uint32_t width, uint32_t height, DWORD style) {
        if (static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)) != style) {
            SetWindowLongW(hwnd_, GWL_STYLE, static_cast<LONG>(style));
        }
        RECT r{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRectExForDpi(&r, style, FALSE, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)),
                                 GetDpiForWindow(hwnd_));
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        pollEvents();
    }

    void updateSize() {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        minimized_ = IsIconic(hwnd_) != FALSE;
        size_ = minimized_ ? Size{0, 0}
                           : Size{static_cast<uint32_t>(rc.right - rc.left), static_cast<uint32_t>(rc.bottom - rc.top)};
    }

    void push(InputEvent e) { events_.push_back(e); }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CLOSE: close_ = true; return 0;
        case WM_SIZE: updateSize(); return 0;
        case WM_GETMINMAXINFO: {
            // Allow a 1 x 1 client area (the default minimum tracking size is larger).
            RECT r{0, 0, 1, 1};
            AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)), FALSE,
                                     static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)), GetDpiForWindow(hwnd_));
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* s = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, s->left, s->top, s->right - s->left, s->bottom - s->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            InputEvent e;
            e.type = InputEvent::Type::MouseMove;
            e.x = static_cast<float>(static_cast<short>(LOWORD(lp)));
            e.y = static_cast<float>(static_cast<short>(HIWORD(lp)));
            push(e);
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
            InputEvent e;
            e.type = InputEvent::Type::MouseButton;
            e.button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1 : 2;
            e.down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
            if (e.down) {
                SetCapture(hwnd_);
            } else if (GetCapture() == hwnd_) {
                ReleaseCapture();
            }
            push(e);
            return 0;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            InputEvent e;
            e.type = InputEvent::Type::MouseWheel;
            const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA);
            (msg == WM_MOUSEWHEEL ? e.y : e.x) = msg == WM_MOUSEWHEEL ? notches : -notches;
            push(e);
            return 0;
        }
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
            const Key k = mapKey(wp);
            if (k != Key::None) {
                InputEvent e;
                e.type = InputEvent::Type::Key;
                e.key = k;
                e.down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
                push(e);
            }
            break; // DefWindowProc still handles system keys (Alt+F4)
        }
        case WM_CHAR: {
            if (wp >= 32 && wp != 127 && !(wp >= 0xD800 && wp <= 0xDFFF)) {
                InputEvent e;
                e.type = InputEvent::Type::Char;
                e.ch = static_cast<uint32_t>(wp);
                push(e);
            }
            return 0;
        }
        case WM_SETFOCUS: case WM_KILLFOCUS: {
            InputEvent e;
            e.type = InputEvent::Type::Focus;
            e.down = msg == WM_SETFOCUS;
            push(e);
            return 0;
        }
        default: break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto* self = static_cast<Win32Window*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    Size size_;
    bool minimized_ = false;
    bool close_ = false;
    std::vector<InputEvent> events_;
};

} // namespace

rhi::Result<std::unique_ptr<Window>> Window::create(const WindowDesc& desc) {
    auto w = std::make_unique<Win32Window>();
    if (auto r = w->init(desc); !r) {
        return std::unexpected(r.error());
    }
    return std::unique_ptr<Window>(std::move(w));
}

} // namespace eng::platform
