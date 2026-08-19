#pragma once

#include <chrono>
#include <deque>
#include <unordered_map>

#include "preview.hpp"

struct ImFont;

namespace pb {

struct Fonts {
    ImFont* ui = nullptr;
    ImFont* mono = nullptr;
};

// Owns the file list, the GPU preview cache and all drawing. Everything in
// here runs on the render thread and must stay well inside the frame budget.
class Viewer {
public:
    Viewer(std::filesystem::path dir, std::size_t budgetBytes, Fonts fonts);

    void frame(float dt);
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return dir_; }

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

    void openDirectory(std::filesystem::path dir);
    void handleInput();
    void pumpLoader();
    void select(int index);
    void prefetch();
    void insert(Preview&& preview);
    void trim();
    [[nodiscard]] Entry* lookup(const FileEntry& file);

    void drawGrid();
    void drawTile(int index, float width, float height);
    void drawOverlay();
    void drawStats();

    std::filesystem::path dir_;
    std::vector<FileEntry> files_;
    Fonts fonts_;
    PreviewLoader loader_;

    std::unordered_map<std::filesystem::path::string_type, Entry> cache_;
    std::deque<Preview> staged_;  // decoded, waiting for a GPU upload slot
    std::size_t cacheBytes_ = 0;
    std::size_t budget_;
    std::uint64_t tick_ = 0;

    int selected_ = 0;
    int columns_ = 1;
    bool open_ = false;
    bool scrollToSelection_ = true;
    float anim_ = 0.0f;

    std::chrono::steady_clock::time_point requestedAt_{};
    bool awaitingContent_ = false;
    double lastOpenMs_ = 0.0;
    bool lastOpenWasHit_ = false;
    std::deque<float> frameTimes_;
    bool showStats_ = true;
};

}  // namespace pb
