#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <shellapi.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <thread>
#include <filesystem>

#include "media.hpp"
#include "overlay.hpp"
#include "resource.hpp"
#include "shell.hpp"
#include "startup.hpp"

namespace {

constexpr std::size_t kBudgetBytes = 512u << 20;
// A preview panel is static apart from a short fade, so it has no business
// burning a high refresh display's worth of frames.
constexpr auto kFrameBudget = std::chrono::microseconds(1'000'000 / 60);
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayAutostart = 1;
constexpr UINT kTrayExit = 2;

// Written by the keyboard hook, consumed by the event loop.
std::atomic<bool> gToggle{false};
std::atomic<bool> gClose{false};
std::atomic<bool> gPlayPause{false};
std::atomic<bool> gOverlayOpen{false};
bool gSwallowedSpace = false;
bool gSwallowedEnter = false;

// Written by the mouse hook: wheel notches collected over the panel, and the
// panel rectangle it tests them against.
std::atomic<int> gWheel{0};
std::atomic<bool> gGrabWheel{false};
std::atomic<LONG> gPanelLeft{0};
std::atomic<LONG> gPanelTop{0};
std::atomic<LONG> gPanelRight{0};
std::atomic<LONG> gPanelBottom{0};

NOTIFYICONDATAW gTray{};

// Runs on the message loop thread, so it must stay short: it only decides
// whether the key belongs to us and hands the work over via a flag.
LRESULT CALLBACK keyboardHook(int code, WPARAM message, LPARAM data) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, message, data);

    const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    const bool overlayOpen = gOverlayOpen.load(std::memory_order_relaxed);

    if (key.vkCode == VK_SPACE) {
        if (down && (overlayOpen || pb::shell::explorerHasFocus())) {
            gToggle.store(true, std::memory_order_relaxed);
            gSwallowedSpace = true;
            return 1;  // Explorer must not also act on this space
        }
        if (up && gSwallowedSpace) {
            gSwallowedSpace = false;
            return 1;
        }
    }
    if (key.vkCode == VK_RETURN && overlayOpen) {
        if (down) {
            gPlayPause.store(true, std::memory_order_relaxed);
            gSwallowedEnter = true;
            return 1;  // Explorer must not open the file behind our back
        }
        if (up && gSwallowedEnter) {
            gSwallowedEnter = false;
            return 1;
        }
    }
    if (key.vkCode == VK_ESCAPE && down && overlayOpen) {
        gClose.store(true, std::memory_order_relaxed);
        return 1;
    }
    return CallNextHookEx(nullptr, code, message, data);
}

// The wheel would otherwise go to whatever has keyboard focus - Explorer, never
// us, because the panel refuses focus by design. So we take it at the source,
// but only for previews that do something with it: media and PDFs. Text
// previews are left to the shell's own inactive-window scrolling.
LRESULT CALLBACK mouseHook(int code, WPARAM message, LPARAM data) {
    if (code != HC_ACTION || message != WM_MOUSEWHEEL || !gGrabWheel.load(std::memory_order_relaxed))
        return CallNextHookEx(nullptr, code, message, data);

    const auto& mouse = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
    const bool overPanel = mouse.pt.x >= gPanelLeft.load(std::memory_order_relaxed) &&
                           mouse.pt.x < gPanelRight.load(std::memory_order_relaxed) &&
                           mouse.pt.y >= gPanelTop.load(std::memory_order_relaxed) &&
                           mouse.pt.y < gPanelBottom.load(std::memory_order_relaxed);
    if (!overPanel) return CallNextHookEx(nullptr, code, message, data);

    gWheel.fetch_add(GET_WHEEL_DELTA_WPARAM(mouse.mouseData) / WHEEL_DELTA, std::memory_order_relaxed);
    return 1;  // and Explorer must not scroll its view underneath us
}

LRESULT CALLBACK trayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message != kTrayMessage || (lparam != WM_RBUTTONUP && lparam != WM_LBUTTONUP))
        return DefWindowProcW(window, message, wparam, lparam);

    const bool autostart = pb::startup::enabled();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (autostart ? MF_CHECKED : MF_UNCHECKED), kTrayAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit PeekaBoo");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);  // required, otherwise the menu never closes
    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu);

    if (choice == kTrayExit) PostQuitMessage(0);
    if (choice == kTrayAutostart && !pb::startup::setEnabled(!autostart))
        MessageBoxW(window, L"The autostart entry could not be changed.", L"PeekaBoo", MB_ICONWARNING | MB_OK);
    return 0;
}

// The notification area wants a small icon; asking for the exact metric keeps
// Windows from scaling the 256 px frame down itself.
HICON loadAppIcon(HINSTANCE instance, int size) {
    if (HICON icon = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(IDI_PEEKABOO), IMAGE_ICON, size, size, LR_DEFAULTCOLOR)))
        return icon;
    return LoadIconW(nullptr, IDI_APPLICATION);
}

HWND createTrayIcon(HINSTANCE instance) {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = trayProc;
    description.hInstance = instance;
    description.lpszClassName = L"PeekaBooTray";
    description.hIcon = loadAppIcon(instance, GetSystemMetrics(SM_CXICON));
    description.hIconSm = loadAppIcon(instance, GetSystemMetrics(SM_CXSMICON));
    RegisterClassExW(&description);

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, description.lpszClassName, L"PeekaBoo", 0, 0, 0, 0, 0, nullptr,
                                  nullptr, instance, nullptr);
    if (!window) return nullptr;

    gTray.cbSize = sizeof(gTray);
    gTray.hWnd = window;
    gTray.uID = 1;
    gTray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gTray.uCallbackMessage = kTrayMessage;
    gTray.hIcon = loadAppIcon(instance, GetSystemMetrics(SM_CXSMICON));
    std::wcsncpy(gTray.szTip, L"PeekaBoo - select a file in Explorer and press Space", std::size(gTray.szTip) - 1);
    Shell_NotifyIconW(NIM_ADD, &gTray);
    return window;
}

int fail(const wchar_t* message) {
    MessageBoxW(nullptr, message, L"PeekaBoo", MB_ICONERROR | MB_OK);
    return 1;
}

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ChildRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.ScrollbarSize = 12.0f;
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.075f, 0.09f, 0.78f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.24f, 0.25f, 0.29f, 1.0f);
}

// ImGui ships stb_truetype, so a system font costs us nothing but a path.
ImFont* loadFont(const char* path, float size) {
    if (!std::filesystem::exists(path)) return nullptr;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size);
}

pb::Fonts loadFonts() {
    pb::Fonts fonts;
    fonts.ui = loadFont("C:/Windows/Fonts/segoeui.ttf", 20.0f);
    fonts.mono = loadFont("C:/Windows/Fonts/consola.ttf", 15.0f);
    if (!fonts.ui) ImGui::GetIO().Fonts->AddFontDefault();
    return fonts;
}

GLFWwindow* createOverlayWindow() {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(1000, 720, "PeekaBoo", nullptr, nullptr);
    if (!window) return nullptr;

    // Out of the taskbar, and a click must never pull focus away from Explorer.
    HWND native = glfwGetWin32Window(window);
    SetWindowLongPtrW(native, GWL_EXSTYLE,
                      GetWindowLongPtrW(native, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(native, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return window;
}

}  // namespace

int main() {
    // Without this, sleep_until rounds up to the 15.6 ms scheduler tick and the
    // frame cap lands nowhere near its target.
    timeBeginPeriod(1);
    if (!pb::shell::initialize()) return fail(L"COM could not be initialised.");
    if (!pb::media::initialize()) return fail(L"Media Foundation could not be initialised.");
    if (!glfwInit()) return fail(L"GLFW could not be initialised.");

    GLFWwindow* window = createOverlayWindow();
    if (!window) return fail(L"The preview window could not be created.");

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);  // the frame limiter below sets the pace, not the display

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    applyStyle();
    const pb::Fonts fonts = loadFonts();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND tray = createTrayIcon(instance);
    HWND native = glfwGetWin32Window(window);

    // WS_EX_TOOLWINDOW alone does not reliably keep the panel out of the taskbar
    // once the shell has seen the window. An owned window never gets a button at
    // all, so the hidden tray window adopts it.
    if (tray) SetWindowLongPtrW(native, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(tray));
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, instance, 0);
    if (!hook) return fail(L"The keyboard hook could not be installed.");
    HHOOK wheel = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, instance, 0);  // volume; not worth failing over

    {
        pb::Overlay overlay(window, kBudgetBytes, fonts);
        auto previous = std::chrono::steady_clock::now();

        while (!glfwWindowShouldClose(window)) {
            const auto frameStart = std::chrono::steady_clock::now();

            if (overlay.open())
                glfwPollEvents();
            else
                glfwWaitEventsTimeout(0.05);

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - previous).count(), 0.1f);
            previous = now;

            if (gToggle.exchange(false)) {
                if (overlay.open())
                    overlay.close();
                else if (const auto file = pb::shell::selectedFile())
                    overlay.show(*file);
            }
            if (gClose.exchange(false)) overlay.close();
            if (gPlayPause.exchange(false)) overlay.togglePlayback();
            if (const int notches = gWheel.exchange(0)) overlay.wheel(notches);

            if (overlay.open()) {  // arrow keys stay in Explorer, we just follow its selection
                if (const auto file = pb::shell::selectedFile(); file && *file != overlay.file()) overlay.show(*file);
            } else {
                pb::shell::forget();
            }
            gOverlayOpen.store(overlay.open(), std::memory_order_relaxed);
            gGrabWheel.store(overlay.open() && overlay.wantsWheel(), std::memory_order_relaxed);

            if (RECT panel{}; overlay.open() && GetWindowRect(native, &panel)) {
                gPanelLeft.store(panel.left, std::memory_order_relaxed);
                gPanelTop.store(panel.top, std::memory_order_relaxed);
                gPanelRight.store(panel.right, std::memory_order_relaxed);
                gPanelBottom.store(panel.bottom, std::memory_order_relaxed);
            }

            if (!overlay.open()) continue;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            overlay.frame(dt);

            ImGui::Render();
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(0.078f, 0.082f, 0.106f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            std::this_thread::sleep_until(frameStart + kFrameBudget);
        }
    }  // the overlay must release its textures while the context is still alive

    UnhookWindowsHookEx(hook);
    if (wheel) UnhookWindowsHookEx(wheel);
    Shell_NotifyIconW(NIM_DELETE, &gTray);
    if (tray) DestroyWindow(tray);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    pb::media::shutdown();
    pb::shell::shutdown();
    timeEndPeriod(1);
    return 0;
}
