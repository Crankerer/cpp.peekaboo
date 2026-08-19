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
#include "shell.hpp"

namespace {

constexpr std::size_t kBudgetBytes = 512u << 20;
// A preview panel is static apart from a short fade, so it has no business
// burning a high refresh display's worth of frames.
constexpr auto kFrameBudget = std::chrono::microseconds(1'000'000 / 60);
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayExit = 1;

// Written by the keyboard hook, consumed by the event loop.
std::atomic<bool> gToggle{false};
std::atomic<bool> gClose{false};
std::atomic<bool> gPlayPause{false};
std::atomic<bool> gOverlayOpen{false};
bool gSwallowedSpace = false;
bool gSwallowedEnter = false;

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

LRESULT CALLBACK trayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message != kTrayMessage || (lparam != WM_RBUTTONUP && lparam != WM_LBUTTONUP))
        return DefWindowProcW(window, message, wparam, lparam);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit PeekaBoo");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);  // required, otherwise the menu never closes
    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu);

    if (choice == kTrayExit) PostQuitMessage(0);
    return 0;
}

HWND createTrayIcon(HINSTANCE instance) {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = trayProc;
    description.hInstance = instance;
    description.lpszClassName = L"PeekaBooTray";
    RegisterClassExW(&description);

    HWND window = CreateWindowExW(0, description.lpszClassName, L"PeekaBoo", 0, 0, 0, 0, 0, nullptr, nullptr, instance,
                                  nullptr);
    if (!window) return nullptr;

    gTray.cbSize = sizeof(gTray);
    gTray.hWnd = window;
    gTray.uID = 1;
    gTray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gTray.uCallbackMessage = kTrayMessage;
    gTray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, instance, 0);
    if (!hook) return fail(L"The keyboard hook could not be installed.");

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

            if (overlay.open()) {  // arrow keys stay in Explorer, we just follow its selection
                if (const auto file = pb::shell::selectedFile(); file && *file != overlay.file()) overlay.show(*file);
            } else {
                pb::shell::forget();
            }
            gOverlayOpen.store(overlay.open(), std::memory_order_relaxed);

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
