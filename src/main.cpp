#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string_view>

#include "viewer.hpp"

namespace {

constexpr std::size_t kDefaultBudgetMB = 512;

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.ScrollbarSize = 12.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.078f, 0.082f, 0.094f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.098f, 0.102f, 0.118f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.23f, 0.27f, 1.0f);
}

// ImGui ships stb_truetype, so a system font costs us nothing but a path.
ImFont* loadFont(const char* path, float size) {
    if (!std::filesystem::exists(path)) return nullptr;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size);
}

pb::Fonts loadFonts() {
    pb::Fonts fonts;
#ifdef _WIN32
    fonts.ui = loadFont("C:/Windows/Fonts/segoeui.ttf", 19.0f);
    fonts.mono = loadFont("C:/Windows/Fonts/consola.ttf", 15.0f);
#else
    fonts.ui = loadFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18.0f);
    fonts.mono = loadFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 15.0f);
#endif
    if (!fonts.ui) ImGui::GetIO().Fonts->AddFontDefault();
    return fonts;
}

std::size_t parseBudgetMB(std::string_view text) {
    std::size_t value = 0;
    const auto [_, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && value > 0 ? value : kDefaultBudgetMB;
}

std::optional<std::filesystem::path> resolveDirectory(int argc, char** argv) {
    std::error_code ec;
    if (argc > 1) {
        std::filesystem::path dir(argv[1]);
        if (std::filesystem::is_directory(dir, ec)) return dir;
        std::fprintf(stderr, "not a directory: %s\n", argv[1]);
        return std::nullopt;
    }
    return std::filesystem::current_path(ec);
}

}  // namespace

int main(int argc, char** argv) {
    const auto dir = resolveDirectory(argc, argv);
    if (!dir) return 1;
    const std::size_t budget = (argc > 2 ? parseBudgetMB(argv[2]) : kDefaultBudgetMB) << 20;

    glfwSetErrorCallback([](int code, const char* message) { std::fprintf(stderr, "glfw %d: %s\n", code, message); });
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 820, "PeekaBoo", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini next to the binary
    applyStyle();
    const pb::Fonts fonts = loadFonts();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    {
        pb::Viewer viewer(*dir, budget, fonts);
        auto previous = std::chrono::steady_clock::now();

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - previous).count();
            previous = now;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            viewer.frame(std::min(dt, 0.1f));

            ImGui::Render();
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(0.078f, 0.082f, 0.094f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
    }  // the viewer must release its textures while the context is still alive

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
