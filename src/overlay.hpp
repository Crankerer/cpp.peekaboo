#pragma once

#include <chrono>
#include <deque>
#include <unordered_map>

#include <imgui.h>

#include "preview.hpp"

struct GLFWwindow;

namespace pb {

struct Fonts {
    ImFont* ui = nullptr;
    ImFont* mono = nullptr;
};

// The floating preview panel. Owns the GPU cache and drives the worker pool;
// everything in here runs on the render thread and stays inside the frame budget.
class Overlay {
public:
    Overlay(GLFWwindow* window, std::size_t budgetBytes, Fonts fonts);

    void show(const std::filesystem::path& file);  // opens, or switches to another file
    void close();

    [[nodiscard]] bool open() const noexcept { return open_; }
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return current_.path; }

    void frame(float dt);  // build the ImGui frame; call between NewFrame and Render

private:
    struct Texture {  // RAII around a GL texture name
        unsigned id = 0;
        int width = 0;
        int height = 0;

        Texture() = default;
        explicit Texture(const ImageData& image);
        ~Texture();
        Texture(Texture&& other) noexcept;
        Texture& operator=(Texture&& other) noexcept;
        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        [[nodiscard]] bool valid() const noexcept { return id != 0; }
    };

    struct Entry {
        Preview preview;
        Texture texture;
        std::uint64_t used = 0;
    };

    void pumpLoader();
    void prefetch();
    void insert(Preview&& preview);
    void trim();
    [[nodiscard]] Entry* lookup(const FileEntry& file);
    void layoutWindow();
    void captureBackdrop();

    void drawHeader(const Entry* entry, float titleLimit);
    [[nodiscard]] float drawOpenWith(const ImVec2& panelMin, const ImVec2& panelMax);
    void drawContent(const Entry* entry, const ImVec2& min, const ImVec2& max, float ease);
    void drawFooter(const Entry* entry, const ImVec2& min, const ImVec2& max, float ease);

    GLFWwindow* window_;
    Fonts fonts_;
    PreviewLoader loader_;

    std::unordered_map<std::filesystem::path::string_type, Entry> cache_;
    std::deque<Preview> staged_;  // decoded, waiting for a GPU upload slot
    std::size_t cacheBytes_ = 0;
    std::size_t budget_;
    std::uint64_t tick_ = 0;

    FileEntry current_;
    std::filesystem::path siblingDir_;
    std::vector<FileEntry> siblings_;  // the folder listing, for neighbour prefetch

    Texture backdrop_;        // blurred snapshot of whatever is behind the panel
    ImVec2 backdropOrigin_{};  // screen region that snapshot covers
    ImVec2 backdropSize_{};
    int windowX_ = 0;  // window rectangle in screen coordinates
    int windowY_ = 0;
    int windowWidth_ = 0;
    int windowHeight_ = 0;
    bool open_ = false;
    float anim_ = 0.0f;

    std::chrono::steady_clock::time_point requestedAt_{};
    bool awaitingContent_ = false;
    double lastOpenMs_ = 0.0;
    bool lastOpenWasHit_ = false;
    std::deque<float> frameTimes_;
};

}  // namespace pb
