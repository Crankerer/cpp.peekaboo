#define IMGUI_DEFINE_MATH_OPERATORS

#include "overlay.hpp"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>  // also provides the GL 1.1 entry points we use
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <format>
#include <iterator>
#include <utility>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace fs = std::filesystem;

namespace pb {
namespace {

constexpr ImU32 kText = IM_COL32(228, 230, 236, 255);
constexpr ImU32 kMuted = IM_COL32(138, 143, 156, 255);
constexpr ImU32 kPanel = IM_COL32(26, 27, 33, 255);
constexpr ImU32 kBorder = IM_COL32(70, 74, 88, 255);

constexpr int kCornerRadius = 20;
constexpr int kUploadsPerFrame = 2;
constexpr std::size_t kFrameHistory = 180;
constexpr std::array kPrefetchOffsets{1, -1, 2, -2};

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

// Fits a width:height box into an area and returns its top-left corner plus size.
std::pair<ImVec2, ImVec2> fitInto(const ImVec2& areaMin, const ImVec2& areaMax, int width, int height) {
    const ImVec2 area = areaMax - areaMin;
    const float scale = std::min(area.x / static_cast<float>(width), area.y / static_cast<float>(height));
    const ImVec2 size(width * scale, height * scale);
    return {areaMin + (area - size) * 0.5f, size};
}

void centeredText(ImFont* font, float size, const ImVec2& areaMin, const ImVec2& areaMax, float y, ImU32 color,
                  const std::string& text) {
    const float width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(font, size, ImVec2((areaMin.x + areaMax.x - width) * 0.5f, y), color,
                                        text.c_str());
}

}  // namespace

// --- GPU texture ------------------------------------------------------------
Overlay::Texture::Texture(const ImageData& image) : width(image.width), height(image.height) {
    static const auto generateMipmap =
        reinterpret_cast<void(APIENTRY*)(GLenum)>(glfwGetProcAddress("glGenerateMipmap"));

    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (generateMipmap) {  // previews are minified a lot, they need mips
        generateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
}

Overlay::Texture::~Texture() {
    if (id) glDeleteTextures(1, &id);
}

Overlay::Texture::Texture(Texture&& other) noexcept : id(other.id), width(other.width), height(other.height) {
    other.id = 0;
}

Overlay::Texture& Overlay::Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (id) glDeleteTextures(1, &id);
        id = std::exchange(other.id, 0u);
        width = other.width;
        height = other.height;
    }
    return *this;
}

// --- lifetime ---------------------------------------------------------------
Overlay::Overlay(GLFWwindow* window, std::size_t budgetBytes, Fonts fonts)
    : window_(window), fonts_(fonts), budget_(budgetBytes) {}

void Overlay::show(const fs::path& file) {
    const bool sameFile = open_ && current_.path == file;
    if (sameFile) return;

    current_ = describe(file);
    requestedAt_ = std::chrono::steady_clock::now();
    awaitingContent_ = true;
    lastOpenWasHit_ = lookup(current_) != nullptr;
    if (!lastOpenWasHit_) loader_.request(current_, true);

    if (const fs::path parent = file.parent_path(); parent != siblingDir_) {
        siblingDir_ = parent;
        siblings_ = scanDirectory(parent);
    }
    prefetch();

    if (!open_) {
        open_ = true;
        placeWindow();
        glfwShowWindow(window_);
    }
}

void Overlay::close() {
    open_ = false;
    anim_ = 0.0f;
    glfwHideWindow(window_);
}

void Overlay::placeWindow() {
    HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;

    const int areaWidth = info.rcWork.right - info.rcWork.left;
    const int areaHeight = info.rcWork.bottom - info.rcWork.top;
    const int width = std::max(760, static_cast<int>(areaWidth * 0.66));
    const int height = std::max(560, static_cast<int>(areaHeight * 0.74));

    glfwSetWindowSize(window_, width, height);
    glfwSetWindowPos(window_, info.rcWork.left + (areaWidth - width) / 2, info.rcWork.top + (areaHeight - height) / 2);

    // Rounded corners cut out of the window itself - an opaque window keeps us
    // independent of whether the compositor grants us a transparent framebuffer.
    SetWindowRgn(glfwGetWin32Window(window_), CreateRoundRectRgn(0, 0, width + 1, height + 1, kCornerRadius, kCornerRadius), TRUE);
}

// --- cache ------------------------------------------------------------------
Overlay::Entry* Overlay::lookup(const FileEntry& file) {
    const auto it = cache_.find(file.path.native());
    if (it == cache_.end()) return nullptr;
    it->second.used = ++tick_;
    return &it->second;
}

void Overlay::insert(Preview&& preview) {
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

void Overlay::trim() {
    const auto& pinned = current_.path.native();

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

void Overlay::pumpLoader() {
    for (auto& preview : loader_.collect()) staged_.push_back(std::move(preview));

    int slots = kUploadsPerFrame;
    const auto& key = current_.path.native();  // whatever is on screen gets the first slot
    if (const auto it = std::ranges::find_if(staged_, [&key](const Preview& p) { return p.path.native() == key; });
        it != staged_.end()) {
        insert(std::move(*it));
        staged_.erase(it);
        --slots;
    }

    while (slots-- > 0 && !staged_.empty()) {
        insert(std::move(staged_.front()));
        staged_.pop_front();
    }
}

void Overlay::prefetch() {
    const auto here = std::ranges::find(siblings_, current_.path, &FileEntry::path);
    if (here == siblings_.end()) return;

    const auto index = std::distance(siblings_.begin(), here);
    for (const int offset : kPrefetchOffsets) {
        const auto neighbour = index + offset;
        if (neighbour < 0 || neighbour >= std::ssize(siblings_)) continue;
        const FileEntry& file = siblings_[static_cast<std::size_t>(neighbour)];
        if (!lookup(file)) loader_.request(file, false);
    }
}

// --- drawing ----------------------------------------------------------------
void Overlay::frame(float dt) {
    frameTimes_.push_back(dt * 1000.0f);
    if (frameTimes_.size() > kFrameHistory) frameTimes_.pop_front();

    pumpLoader();

    anim_ += (1.0f - anim_) * (1.0f - std::exp(-dt * 22.0f));  // close() resets it, so this only ever fades in
    if (anim_ > 0.998f) anim_ = 1.0f;
    const float ease = anim_ * anim_ * (3.0f - 2.0f * anim_);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 pos = viewport->Pos;
    const ImVec2 size = viewport->Size;

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 18.0f));
    ImGui::Begin("##preview", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, pos + size, kPanel);
    draw->AddRect(pos + ImVec2(1.0f, 1.0f), pos + size - ImVec2(1.0f, 1.0f), kBorder, kCornerRadius, 0, 1.5f);

    const Entry* entry = lookup(current_);
    drawHeader(entry);

    const ImVec2 contentMin = ImGui::GetCursorScreenPos();
    const ImVec2 contentMax(pos.x + size.x - 22.0f, pos.y + size.y - 44.0f);
    drawContent(entry, contentMin, contentMax, ease);
    drawFooter(entry, contentMin, contentMax, ease);

    if (entry && awaitingContent_) {
        lastOpenMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requestedAt_).count();
        awaitingContent_ = false;
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void Overlay::drawHeader(const Entry* entry) {
    if (fonts_.ui) ImGui::PushFont(fonts_.ui);
    ImGui::TextUnformatted(current_.name.c_str());
    if (fonts_.ui) ImGui::PopFont();

    std::string meta = std::format("{}   {}", current_.ext.empty() ? "file" : current_.ext, humanSize(current_.size));
    if (entry) {
        if (const auto* image = std::get_if<ImageData>(&entry->preview.content))
            meta += std::format("   {} x {} px", image->sourceWidth, image->sourceHeight);
        meta += std::format("   decoded in {:.2f} ms", entry->preview.loadMs);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(meta.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void Overlay::drawContent(const Entry* entry, const ImVec2& min, const ImVec2& max, float ease) {
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (!entry) {
        centeredText(ImGui::GetFont(), 16.0f, min, max, (min.y + max.y) * 0.5f, fade(kMuted, ease), "decoding...");
        return;
    }

    if (const auto* image = std::get_if<ImageData>(&entry->preview.content); image && entry->texture.valid()) {
        const auto [imagePos, imageSize] = fitInto(min, max, entry->texture.width, entry->texture.height);
        const ImVec2 grown = imageSize * (0.97f + 0.03f * ease);  // subtle pop-in
        const ImVec2 origin = imagePos + (imageSize - grown) * 0.5f;
        draw->AddImageRounded(textureId(entry->texture.id), origin, origin + grown, ImVec2(0, 0), ImVec2(1, 1),
                              fade(IM_COL32_WHITE, ease), 8.0f);
        return;
    }

    if (const auto* text = std::get_if<TextData>(&entry->preview.content)) {
        if (fonts_.mono) ImGui::PushFont(fonts_.mono);
        ImGui::BeginChild("##text", max - min, 0, ImGuiWindowFlags_HorizontalScrollbar);

        ImGuiListClipper clipper;  // a 4 MB log costs the same as a two line note
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
        return;
    }

    const ImVec2 badgeMin((min.x + max.x) * 0.5f - 60.0f, (min.y + max.y) * 0.5f - 80.0f);
    const ImVec2 badgeMax(badgeMin.x + 120.0f, badgeMin.y + 120.0f);
    draw->AddRectFilled(badgeMin, badgeMax, fade(kindColor(current_.kind), ease), 12.0f);
    centeredText(ImGui::GetFont(), 24.0f, badgeMin, badgeMax, badgeMin.y + 46.0f, fade(kText, ease),
                 current_.ext.empty() ? "FILE" : current_.ext);

    const auto* info = std::get_if<InfoData>(&entry->preview.content);
    centeredText(ImGui::GetFont(), 16.0f, min, max, badgeMax.y + 20.0f, fade(kMuted, ease),
                 info ? info->note : std::string{"No preview available"});
}

void Overlay::drawFooter(const Entry* entry, const ImVec2& min, const ImVec2& max, float ease) {
    float average = 0.0f;
    for (const float value : frameTimes_) average += value;
    if (!frameTimes_.empty()) average /= static_cast<float>(frameTimes_.size());

    const auto* text = entry ? std::get_if<TextData>(&entry->preview.content) : nullptr;
    const std::string footer =
        std::format("Space / Esc close     arrows browse     open {:.2f} ms ({})     {:.0f} FPS     cache {} / {}{}",
                    lastOpenMs_, lastOpenWasHit_ ? "prefetched" : "decoded", average > 0.0f ? 1000.0f / average : 0.0f,
                    humanSize(cacheBytes_), humanSize(budget_), text && text->truncated ? "     truncated at 1 MiB" : "");

    centeredText(ImGui::GetFont(), 13.0f, min, max, max.y + 8.0f, fade(kMuted, ease), footer);
}

}  // namespace pb
