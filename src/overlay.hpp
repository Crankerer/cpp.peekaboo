#pragma once

#include <chrono>
#include <deque>
#include <memory>
#include <unordered_map>

#include <imgui.h>

#include "media.hpp"
#include "pdf.hpp"
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
    void togglePlayback();
    void skipPlayback(double seconds);  // relative jump, negative goes back
    void wheel(int notches);            // volume on a media preview, scrolling on a PDF

    [[nodiscard]] bool open() const noexcept { return open_; }
    // Whether the wheel belongs to us rather than to whatever has focus.
    [[nodiscard]] bool wantsWheel() const noexcept { return player_ != nullptr || pdf_ != nullptr; }
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return current_.path; }

    void frame(float dt);  // build the ImGui frame; call between NewFrame and Render

private:
    struct Texture {  // RAII around a GL texture name
        unsigned id = 0;
        int width = 0;
        int height = 0;

        Texture() = default;
        explicit Texture(const ImageData& image);
        Texture(int width, int height);  // empty, refilled every frame by the player
        ~Texture();
        Texture(Texture&& other) noexcept;
        Texture& operator=(Texture&& other) noexcept;
        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        [[nodiscard]] bool valid() const noexcept { return id != 0; }
        void write(const std::uint8_t* bgra) const;
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
    void setVolume(float value);

    // Where a point of the panel falls in the monitor snapshot, so a control can
    // be cut out of the same frosted material as the panel background.
    [[nodiscard]] ImVec2 backdropUV(const ImVec2& point) const;
    void drawFrosted(const ImVec2& min, const ImVec2& max, float rounding, float ease);

    void drawHeader(const Entry* entry, float titleLimit);
    [[nodiscard]] float drawOpenWith(const ImVec2& panelMin, const ImVec2& panelMax);
    void drawContent(const Entry* entry, const ImVec2& min, const ImVec2& max, float ease);
    void drawMedia(const Entry* entry, const ImVec2& min, const ImVec2& max, float ease);
    void drawTransport(const ImVec2& min, const ImVec2& max, float ease);  // seek bar, buttons, clock
    void drawVolume(const ImVec2& min, const ImVec2& max, float ease);     // slider at the right edge
    void drawPdf(const ImVec2& min, const ImVec2& max, float ease);        // the scrolling page stack
    void pumpPdf();
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

    std::unique_ptr<media::Player> player_;  // alive only while a media file is on screen
    Texture video_;                          // streaming target for the player's frames
    float volume_ = 1.0f;                    // outlives the player, so it carries from file to file

    std::unique_ptr<pdf::Document> pdf_;  // alive only while a PDF is on screen
    std::vector<pdf::Page> pdfLayout_;    // page sizes, known before anything is rasterised
    std::unordered_map<int, Texture> pdfPages_;  // rasterised pages near the viewport
    float pdfScroll_ = 0.0f;
    int pdfPage_ = 0;  // page the viewport is currently on, for the indicator

    Texture backdrop_;        // blurred snapshot of whatever is behind the panel
    ImVec2 backdropOrigin_{};  // screen region that snapshot covers
    ImVec2 backdropSize_{};
    int windowX_ = 0;  // window rectangle in screen coordinates
    int windowY_ = 0;
    int windowWidth_ = 0;
    int windowHeight_ = 0;
    bool open_ = false;
    float anim_ = 0.0f;
};

}  // namespace pb
