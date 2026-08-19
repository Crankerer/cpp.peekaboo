#define IMGUI_DEFINE_MATH_OPERATORS

#include "viewer.hpp"

#include <imgui.h>
#include <GLFW/glfw3.h>  // also provides the GL 1.1 entry points we use

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <format>
#include <utility>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef APIENTRY
#define APIENTRY
#endif

namespace fs = std::filesystem;

namespace pb {
namespace {

constexpr ImU32 kText = IM_COL32(228, 230, 236, 255);
constexpr ImU32 kMuted = IM_COL32(138, 143, 156, 255);
constexpr ImU32 kTile = IM_COL32(34, 35, 41, 255);
constexpr ImU32 kTileHover = IM_COL32(44, 46, 54, 255);
constexpr ImU32 kTileActive = IM_COL32(38, 56, 84, 255);
constexpr ImU32 kAccent = IM_COL32(96, 165, 250, 255);

constexpr float kTileWidth = 152.0f;
constexpr float kTileHeight = 138.0f;
constexpr float kTileGap = 12.0f;
constexpr int kUploadsPerFrame = 2;  // keeps a burst of decodes from blowing the frame budget
constexpr std::size_t kFrameHistory = 180;

ImU32 kindColor(Kind kind) {
    switch (kind) {
        case Kind::Image: return IM_COL32(45, 122, 118, 255);
        case Kind::Text: return IM_COL32(58, 92, 150, 255);
        case Kind::Other: return IM_COL32(72, 74, 84, 255);
    }
    return kMuted;
}

ImTextureID textureId(unsigned id) { return reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(id)); }

ImU32 fade(ImU32 color, float alpha) {
    const auto a = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

std::string ellipsize(const std::string& text, ImFont* font, float size, float maxWidth) {
    const float dots = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "...").x;
    const char* remaining = nullptr;
    font->CalcTextSizeA(size, maxWidth - dots, 0.0f, text.c_str(), nullptr, &remaining);
    if (remaining == text.c_str() + text.size()) return text;
    return std::string(text.c_str(), remaining) + "...";
}

// Fits a w:h box into the given area and returns the top-left corner plus size.
std::pair<ImVec2, ImVec2> fitInto(const ImVec2& areaMin, const ImVec2& areaMax, int width, int height) {
    const ImVec2 area = areaMax - areaMin;
    const float scale = std::min(area.x / static_cast<float>(width), area.y / static_cast<float>(height));
    const ImVec2 size(width * scale, height * scale);
    return {areaMin + (area - size) * 0.5f, size};
}

void centeredText(ImFont* font, float size, const ImVec2& areaMin, const ImVec2& areaMax, float y, ImU32 color,
                  const std::string& text) {
    const float width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    const ImVec2 pos((areaMin.x + areaMax.x - width) * 0.5f, y);
    ImGui::GetWindowDrawList()->AddText(font, size, pos, color, text.c_str());
}

}  // namespace

// --- GPU texture ------------------------------------------------------------
Viewer::Texture::Texture(const ImageData& image) : width(image.width), height(image.height) {
    static const auto generateMipmap =
        reinterpret_cast<void(APIENTRY*)(GLenum)>(glfwGetProcAddress("glGenerateMipmap"));

    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (generateMipmap) {  // grid thumbnails are heavily minified, they need mips
        generateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
}

Viewer::Texture::~Texture() {
    if (id) glDeleteTextures(1, &id);
}

Viewer::Texture::Texture(Texture&& other) noexcept : id(other.id), width(other.width), height(other.height) {
    other.id = 0;
}

Viewer::Texture& Viewer::Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (id) glDeleteTextures(1, &id);
        id = std::exchange(other.id, 0u);
        width = other.width;
        height = other.height;
    }
    return *this;
}

// --- lifetime ---------------------------------------------------------------
Viewer::Viewer(fs::path dir, std::size_t budgetBytes, Fonts fonts) : fonts_(fonts), budget_(budgetBytes) {
    openDirectory(std::move(dir));
}

void Viewer::openDirectory(fs::path dir) {
    dir_ = std::move(dir);
    files_ = scanDirectory(dir_);
    cache_.clear();
    staged_.clear();
    cacheBytes_ = 0;
    selected_ = 0;
    open_ = false;
    scrollToSelection_ = true;
    if (!files_.empty()) select(0);
}

// --- cache ------------------------------------------------------------------
Viewer::Entry* Viewer::lookup(const FileEntry& file) {
    const auto it = cache_.find(file.path.native());
    if (it == cache_.end()) return nullptr;
    it->second.used = ++tick_;
    return &it->second;
}

void Viewer::insert(Preview&& preview) {
    auto key = preview.path.native();
    if (const auto it = cache_.find(key); it != cache_.end()) cacheBytes_ -= it->second.preview.bytes;

    Entry entry;
    entry.used = ++tick_;
    if (auto* image = std::get_if<ImageData>(&preview.content); image && !image->rgba.empty()) {
        entry.texture = Texture(*image);
        image->rgba = {};  // the pixels live on the GPU now
    }

    cacheBytes_ += preview.bytes;
    entry.preview = std::move(preview);
    cache_.insert_or_assign(std::move(key), std::move(entry));
    trim();
}

void Viewer::trim() {
    const auto& pinned = files_.empty() ? fs::path::string_type{} : files_[selected_].path.native();

    while (cacheBytes_ > budget_ && cache_.size() > 1) {
        auto victim = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->first == pinned) continue;
            if (victim == cache_.end() || it->second.used < victim->second.used) victim = it;
        }
        if (victim == cache_.end()) return;
        cacheBytes_ -= victim->second.preview.bytes;
        cache_.erase(victim);
    }
}

void Viewer::pumpLoader() {
    for (auto& preview : loader_.collect()) staged_.push_back(std::move(preview));

    int slots = kUploadsPerFrame;
    if (!files_.empty()) {  // whatever is on screen gets the first upload slot
        const auto& key = files_[selected_].path.native();
        const auto it = std::ranges::find_if(staged_, [&key](const Preview& p) { return p.path.native() == key; });
        if (it != staged_.end()) {
            insert(std::move(*it));
            staged_.erase(it);
            --slots;
        }
    }

    while (slots-- > 0 && !staged_.empty()) {
        insert(std::move(staged_.front()));
        staged_.pop_front();
    }
}

void Viewer::select(int index) {
    selected_ = index;
    scrollToSelection_ = true;
    if (!lookup(files_[selected_])) loader_.request(files_[selected_], true);
    prefetch();
}

void Viewer::prefetch() {
    static constexpr std::array offsets{1, -1, 2, -2};
    for (const int offset : offsets) {
        const int index = selected_ + offset;
        if (index < 0 || index >= static_cast<int>(files_.size())) continue;
        if (!lookup(files_[index])) loader_.request(files_[index], false);
    }
}

// --- frame ------------------------------------------------------------------
void Viewer::frame(float dt) {
    frameTimes_.push_back(dt * 1000.0f);
    if (frameTimes_.size() > kFrameHistory) frameTimes_.pop_front();

    pumpLoader();
    handleInput();

    const float target = open_ ? 1.0f : 0.0f;
    anim_ += (target - anim_) * (1.0f - std::exp(-dt * 20.0f));
    if (std::abs(target - anim_) < 0.002f) anim_ = target;

    drawGrid();
    if (anim_ > 0.001f) drawOverlay();
    if (showStats_) drawStats();
}

void Viewer::handleInput() {
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) showStats_ = !showStats_;
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        if (const auto picked = pickFolder()) openDirectory(*picked);
        return;
    }
    if (files_.empty()) return;

    const auto startTiming = [this] {
        requestedAt_ = std::chrono::steady_clock::now();
        awaitingContent_ = true;
        lastOpenWasHit_ = lookup(files_[selected_]) != nullptr;
    };

    const int last = static_cast<int>(files_.size()) - 1;
    int target = selected_;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ++target;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) --target;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) target += open_ ? 1 : columns_;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) target -= open_ ? 1 : columns_;
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) target = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) target = last;

    if (const int clamped = std::clamp(target, 0, last); clamped != selected_) {
        select(clamped);
        if (open_) startTiming();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        open_ = !open_;
        if (open_) startTiming();
    }
    if (open_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) open_ = false;
}

// --- grid -------------------------------------------------------------------
void Viewer::drawGrid() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
    ImGui::Begin("##browser", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextUnformatted(toUtf8(dir_).c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::Text("%zu files    Space preview    arrows navigate    O open folder    F1 stats", files_.size());
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::BeginChild("##grid");
    const float available = ImGui::GetContentRegionAvail().x;
    columns_ = std::max(1, static_cast<int>((available + kTileGap) / (kTileWidth + kTileGap)));

    const int count = static_cast<int>(files_.size());
    const int rows = (count + columns_ - 1) / columns_;

    if (scrollToSelection_ && count > 0) {
        scrollToSelection_ = false;
        const float top = static_cast<float>(selected_ / columns_) * (kTileHeight + kTileGap);
        const float bottom = top + kTileHeight;
        const float view = ImGui::GetScrollY();
        const float height = ImGui::GetContentRegionAvail().y;
        if (top < view)
            ImGui::SetScrollY(top);
        else if (bottom > view + height)
            ImGui::SetScrollY(bottom - height);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kTileGap, kTileGap));
    ImGuiListClipper clipper;
    clipper.Begin(rows, kTileHeight + kTileGap);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int column = 0; column < columns_; ++column) {
                const int index = row * columns_ + column;
                if (index >= count) break;
                if (column > 0) ImGui::SameLine();
                drawTile(index, kTileWidth, kTileHeight);
            }
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleVar();
}

void Viewer::drawTile(int index, float width, float height) {
    const FileEntry& file = files_[index];

    ImGui::PushID(index);
    ImGui::InvisibleButton("tile", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) select(index);
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) open_ = true;
    ImGui::PopID();

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool active = index == selected_;

    draw->AddRectFilled(min, max, active ? kTileActive : (hovered ? kTileHover : kTile), 10.0f);
    if (active) draw->AddRect(min, max, kAccent, 10.0f, 0, 2.0f);

    const ImVec2 thumbMin(min.x + 10.0f, min.y + 10.0f);
    const ImVec2 thumbMax(max.x - 10.0f, max.y - 46.0f);

    const Entry* entry = lookup(file);
    if (entry && entry->texture.valid()) {
        const auto [pos, size] = fitInto(thumbMin, thumbMax, entry->texture.width, entry->texture.height);
        draw->AddImageRounded(textureId(entry->texture.id), pos, pos + size, ImVec2(0, 0), ImVec2(1, 1),
                              IM_COL32_WHITE, 6.0f);
    } else {
        draw->AddRectFilled(thumbMin, thumbMax, kindColor(file.kind), 6.0f);
        const std::string badge = file.ext.empty() ? "FILE" : file.ext;
        centeredText(ImGui::GetFont(), 15.0f, thumbMin, thumbMax, (thumbMin.y + thumbMax.y) * 0.5f - 8.0f, kText, badge);
    }

    // Keep the visible page warm, but never at the expense of the selection.
    if (!entry && loader_.pending() < 8) loader_.request(file, false);

    ImFont* font = ImGui::GetFont();
    draw->AddText(font, 14.0f, ImVec2(min.x + 10.0f, max.y - 40.0f), kText,
                  ellipsize(file.name, font, 14.0f, width - 20.0f).c_str());
    draw->AddText(font, 12.0f, ImVec2(min.x + 10.0f, max.y - 22.0f), kMuted, humanSize(file.size).c_str());
}

// --- overlay ----------------------------------------------------------------
void Viewer::drawOverlay() {
    if (files_.empty()) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float ease = anim_ * anim_ * (3.0f - 2.0f * anim_);
    const float scale = 0.94f + 0.06f * ease;
    const ImVec2 size(viewport->WorkSize.x * 0.86f * scale, viewport->WorkSize.y * 0.86f * scale);
    const ImVec2 pos = viewport->WorkPos + (viewport->WorkSize - size) * 0.5f;

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 18.0f));
    ImGui::Begin("##preview", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoNav);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRectFullScreen();
    draw->AddRectFilled(viewport->Pos, viewport->Pos + viewport->Size, IM_COL32(0, 0, 0, static_cast<int>(185 * ease)));
    draw->PopClipRect();
    draw->AddRectFilled(pos, pos + size, fade(IM_COL32(27, 28, 34, 255), ease), 16.0f);
    draw->AddRect(pos, pos + size, fade(IM_COL32(68, 72, 84, 255), ease), 16.0f, 0, 1.5f);

    const FileEntry& file = files_[selected_];
    const Entry* entry = lookup(file);

    if (fonts_.ui) ImGui::PushFont(fonts_.ui);
    ImGui::TextUnformatted(file.name.c_str());
    if (fonts_.ui) ImGui::PopFont();

    std::string meta = std::format("{}   {}", file.ext.empty() ? "file" : file.ext, humanSize(file.size));
    if (entry) {
        if (const auto* image = std::get_if<ImageData>(&entry->preview.content))
            meta += std::format("   {} x {} px", image->width, image->height);
        meta += std::format("   decoded in {:.2f} ms", entry->preview.loadMs);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(meta.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();

    const ImVec2 contentMin = ImGui::GetCursorScreenPos();
    const ImVec2 contentMax(pos.x + size.x - 22.0f, pos.y + size.y - 44.0f);
    const ImVec2 contentSize = contentMax - contentMin;

    if (!entry) {
        centeredText(ImGui::GetFont(), 16.0f, contentMin, contentMax, (contentMin.y + contentMax.y) * 0.5f, kMuted,
                     "decoding...");
    } else if (const auto* image = std::get_if<ImageData>(&entry->preview.content); image && entry->texture.valid()) {
        const auto [imagePos, imageSize] = fitInto(contentMin, contentMax, entry->texture.width, entry->texture.height);
        draw->AddImageRounded(textureId(entry->texture.id), imagePos, imagePos + imageSize, ImVec2(0, 0), ImVec2(1, 1),
                              fade(IM_COL32_WHITE, ease), 8.0f);
    } else if (const auto* text = std::get_if<TextData>(&entry->preview.content)) {
        if (fonts_.mono) ImGui::PushFont(fonts_.mono);
        ImGui::BeginChild("##text", contentSize, 0, ImGuiWindowFlags_HorizontalScrollbar);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(text->lines.size()));
        while (clipper.Step()) {
            for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
                const char* begin = text->text.data() + text->lines[line];
                const char* end = line + 1 < static_cast<int>(text->lines.size())
                                      ? text->text.data() + text->lines[line + 1] - 1
                                      : text->text.data() + text->text.size();
                ImGui::TextUnformatted(begin, std::max(begin, end));
            }
        }
        ImGui::EndChild();
        if (fonts_.mono) ImGui::PopFont();
    } else {
        const ImVec2 badgeMin((contentMin.x + contentMax.x) * 0.5f - 60.0f, (contentMin.y + contentMax.y) * 0.5f - 80.0f);
        const ImVec2 badgeMax(badgeMin.x + 120.0f, badgeMin.y + 120.0f);
        draw->AddRectFilled(badgeMin, badgeMax, fade(kindColor(file.kind), ease), 12.0f);
        centeredText(ImGui::GetFont(), 24.0f, badgeMin, badgeMax, badgeMin.y + 46.0f, fade(kText, ease),
                     file.ext.empty() ? "FILE" : file.ext);

        const auto* info = std::get_if<InfoData>(&entry->preview.content);
        centeredText(ImGui::GetFont(), 16.0f, contentMin, contentMax, badgeMax.y + 20.0f, fade(kMuted, ease),
                     info ? info->note : std::string{"No preview available"});
    }

    const bool truncated = entry && std::holds_alternative<TextData>(entry->preview.content) &&
                           std::get<TextData>(entry->preview.content).truncated;
    centeredText(ImGui::GetFont(), 13.0f, contentMin, contentMax, contentMax.y + 8.0f, fade(kMuted, ease),
                 truncated ? "preview truncated at 1 MiB" : "Space / Esc close        left / right browse");

    if (entry && awaitingContent_) {
        lastOpenMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requestedAt_).count();
        awaitingContent_ = false;
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

// --- stats ------------------------------------------------------------------
void Viewer::drawStats() {
    float average = 0.0f;
    float worst = 0.0f;
    int slow = 0;
    for (const float value : frameTimes_) {
        average += value;
        worst = std::max(worst, value);
        if (value > 16.7f) ++slow;
    }
    if (!frameTimes_.empty()) average /= static_cast<float>(frameTimes_.size());

    const std::array lines{
        std::format("{:.0f} FPS    avg {:.2f} ms    worst {:.2f} ms", average > 0.0f ? 1000.0f / average : 0.0f,
                    average, worst),
        std::format("frames over 16.7 ms: {} / {}", slow, frameTimes_.size()),
        std::format("cache {} / {}    {} entries", humanSize(cacheBytes_), humanSize(budget_), cache_.size()),
        std::format("{} workers    {} pending    {} staged", loader_.workerCount(), loader_.pending(), staged_.size()),
        std::format("last open {:.2f} ms ({})", lastOpenMs_, lastOpenWasHit_ ? "cache hit" : "decoded"),
    };

    ImFont* font = ImGui::GetFont();
    constexpr float size = 13.0f;
    float width = 0.0f;
    for (const auto& line : lines) width = std::max(width, font->CalcTextSizeA(size, FLT_MAX, 0.0f, line.c_str()).x);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = static_cast<float>(lines.size()) * 18.0f + 16.0f;
    const ImVec2 max(viewport->WorkPos.x + viewport->WorkSize.x - 14.0f, viewport->WorkPos.y + 14.0f + height);
    const ImVec2 min(max.x - width - 24.0f, viewport->WorkPos.y + 14.0f);

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(min, max, IM_COL32(16, 17, 20, 225), 8.0f);
    draw->AddRect(min, max, IM_COL32(60, 63, 72, 255), 8.0f);
    for (std::size_t i = 0; i < lines.size(); ++i)
        draw->AddText(font, size, ImVec2(min.x + 12.0f, min.y + 8.0f + static_cast<float>(i) * 18.0f),
                      i == 0 ? kAccent : kMuted, lines[i].c_str());
}

}  // namespace pb
