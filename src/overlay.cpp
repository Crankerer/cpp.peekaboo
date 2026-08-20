#define IMGUI_DEFINE_MATH_OPERATORS

#include "overlay.hpp"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>  // also provides the GL 1.1 entry points we use
#include <GLFW/glfw3native.h>

#include <dwmapi.h>
#include <shellapi.h>

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
#ifndef GL_BGRA
#define GL_BGRA 0x80E1  // lets video frames go to the GPU without a byte swap
#endif

namespace fs = std::filesystem;

namespace pb {
namespace {

constexpr ImU32 kText = IM_COL32(228, 230, 236, 255);
constexpr ImU32 kMuted = IM_COL32(138, 143, 156, 255);
constexpr ImU32 kAccent = IM_COL32(96, 165, 250, 255);
constexpr ImU32 kTrack = IM_COL32(72, 76, 92, 235);
constexpr ImU32 kPanelTint = IM_COL32(20, 21, 27, 178);  // darkens the backdrop so text stays readable

constexpr int kMaxWidth = 1366;  // 768p is the upper bound for the panel
constexpr int kMaxHeight = 768;
constexpr int kMinWidth = 560;
constexpr int kMinHeight = 320;
constexpr int kIconWidth = 520;  // nothing but an icon and a note to show
constexpr int kIconHeight = 460;
constexpr int kUnknownWidth = 960;  // size while the preview is still decoding
constexpr int kUnknownHeight = 640;
constexpr int kMediaWidth = 640;  // audio, and video before its size is known
constexpr int kMediaHeight = 480;
constexpr float kTimelineHeight = 84.0f;  // seek bar, transport buttons and the clock line
constexpr float kVolumeColumn = 44.0f;    // width the volume slider needs to float in
constexpr ImU32 kScrim = IM_COL32(14, 15, 20, 188);  // tint over the frosted control backing
constexpr double kSkipSeconds = 10.0;
constexpr float kVolumeStep = 0.05f;
constexpr float kScrollStep = 120.0f;  // per wheel notch, roughly a paragraph
constexpr float kPageGap = 14.0f;
constexpr int kPagesAround = 3;  // rasterised pages kept either side of the viewport
constexpr int kBlurDivisor = 8;
constexpr float kButtonRounding = 8.0f;  // matches the radius Windows 11 rounds the window with
constexpr ImU32 kButton = IM_COL32(18, 19, 23, 205);  // same weight as the text preview background
constexpr ImU32 kButtonHover = IM_COL32(34, 36, 44, 225);  // how hard the snapshot is shrunk, which is the blur radius
constexpr int kChromeX = 44;   // padding left and right of the content
constexpr int kChromeY = 122;  // title, meta line, separator and footer
constexpr int kUploadsPerFrame = 2;
constexpr std::array kPrefetchOffsets{1, -1, 2, -2};

ImU32 kindColor(Kind kind) {
    switch (kind) {
        case Kind::Image: return IM_COL32(45, 122, 118, 255);
        case Kind::Text: return IM_COL32(58, 92, 150, 255);
        case Kind::Media: return IM_COL32(120, 74, 148, 255);
        case Kind::Pdf: return IM_COL32(150, 64, 64, 255);
        case Kind::Other: return IM_COL32(72, 74, 84, 255);
    }
    return kMuted;
}

// ImTextureID is an integer wide enough for a pointer, so this is a widening
// conversion and not a reinterpretation - MSVC rejects the latter outright.
ImTextureID textureId(unsigned id) { return static_cast<ImTextureID>(id); }

ImU32 fade(ImU32 color, float alpha) {
    const auto a = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// Windows composites this OpenGL window opaquely - it ignores our alpha channel,
// so neither DwmEnableBlurBehindWindow nor the accent policy ever reaches us.
// We build the frosted backdrop ourselves: grab the screen while the panel is
// still hidden and shrink it hard, which is exactly a box blur. Stretching it
// back up adds the smoothing.
ImageData grabScreen(int left, int top, int areaWidth, int areaHeight, int divisor) {
    const int width = std::max(1, areaWidth / divisor);
    const int height = std::max(1, areaHeight / divisor);

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);

    SetStretchBltMode(memory, HALFTONE);  // averages the dropped pixels instead of picking one
    SetBrushOrgEx(memory, 0, 0, nullptr);
    StretchBlt(memory, 0, 0, width, height, screen, left, top, areaWidth, areaHeight, SRCCOPY);

    BITMAPINFO request{};
    request.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    request.bmiHeader.biWidth = width;
    request.bmiHeader.biHeight = -height;  // negative: top-down
    request.bmiHeader.biPlanes = 1;
    request.bmiHeader.biBitCount = 32;
    request.bmiHeader.biCompression = BI_RGB;

    ImageData shot;
    shot.width = width;
    shot.height = height;
    shot.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    const int copied = GetDIBits(memory, bitmap, 0, height, shot.rgba.data(), &request, DIB_RGB_COLORS);

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    if (copied == 0) return {};
    for (std::size_t i = 0; i < shot.rgba.size(); i += 4) {  // GDI hands out BGRX
        std::swap(shot.rgba[i], shot.rgba[i + 2]);
        shot.rgba[i + 3] = 255;
    }
    return shot;
}

// Windows 11 rounds a window for us. A window region would do it too, but that
// opts the window out of DWM per-pixel alpha, which would cost us the blur.
void roundCorners(HWND window) {
    constexpr DWORD kCornerPreference = 33;
    constexpr DWORD kRound = 2;
    DwmSetWindowAttribute(window, kCornerPreference, &kRound, sizeof(kRound));
}

std::string ellipsize(const std::string& text, ImFont* font, float size, float maxWidth) {
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= maxWidth) return text;

    const float dots = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "...").x;
    const char* remaining = nullptr;
    font->CalcTextSizeA(size, std::max(0.0f, maxWidth - dots), 0.0f, text.c_str(), nullptr, &remaining);
    return std::string(text.c_str(), remaining) + "...";
}

std::string clockText(double seconds) {
    if (seconds < 0.0 || seconds > 360000.0) seconds = 0.0;
    const auto total = static_cast<int>(seconds);
    return std::format("{}:{:02}", total / 60, total % 60);
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

struct Hit {
    bool clicked = false;
    bool hovered = false;
    bool active = false;  // held down, which is what makes a slider drag
};

// The panel paints through the draw list, so a control that wants the mouse has
// to borrow the ImGui cursor and hand it straight back.
Hit hitTest(const char* id, const ImVec2& min, const ImVec2& max) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)));
    const Hit state{ImGui::IsItemClicked(), ImGui::IsItemHovered(), ImGui::IsItemActive()};
    ImGui::SetCursorScreenPos(cursor);
    return state;
}

// Transport glyphs. No icon font is loaded, and these are simple enough that
// asking one to exist would cost more than drawing them.
void glyphPlay(ImDrawList* draw, const ImVec2& c, ImU32 color) {
    draw->AddTriangleFilled(ImVec2(c.x + 8.0f, c.y), ImVec2(c.x - 6.0f, c.y + 9.0f), ImVec2(c.x - 6.0f, c.y - 9.0f),
                            color);
}

void glyphPause(ImDrawList* draw, const ImVec2& c, ImU32 color) {
    draw->AddRectFilled(ImVec2(c.x - 7.0f, c.y - 9.0f), ImVec2(c.x - 2.0f, c.y + 9.0f), color, 1.5f);
    draw->AddRectFilled(ImVec2(c.x + 2.0f, c.y - 9.0f), ImVec2(c.x + 7.0f, c.y + 9.0f), color, 1.5f);
}

// Two chevrons; direction is +1 forward, -1 back. The winding follows the
// direction so the anti-aliased edge stays on the outside either way.
void glyphSkip(ImDrawList* draw, const ImVec2& c, float direction, ImU32 color) {
    for (const float offset : {-7.0f, 1.0f}) {
        const float base = c.x + direction * offset;
        const float tip = c.x + direction * (offset + 6.0f);
        draw->AddTriangleFilled(ImVec2(tip, c.y), ImVec2(base, c.y + direction * 8.0f),
                                ImVec2(base, c.y - direction * 8.0f), color);
    }
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

Overlay::Texture::Texture(int width, int height) : width(width), height(height) {
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // no mips, this is rewritten every frame
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void Overlay::Texture::write(const std::uint8_t* bgra) const {
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
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
    if (!lookup(current_)) loader_.request(current_, true);

    player_.reset();  // whatever was playing belongs to the file we just left
    video_ = Texture{};
    if (current_.kind == Kind::Media) {
        player_ = std::make_unique<media::Player>(current_.path);
        player_->setVolume(volume_);  // the level the user last set, not full blast
    }

    pdf_.reset();
    pdfLayout_.clear();
    pdfPages_.clear();
    pdfScroll_ = 0.0f;
    pdfPage_ = 0;
    if (current_.kind == Kind::Pdf) pdf_ = std::make_unique<pdf::Document>(current_.path);

    if (const fs::path parent = file.parent_path(); parent != siblingDir_) {
        siblingDir_ = parent;
        siblings_ = scanDirectory(parent);
    }
    prefetch();
    layoutWindow();

    if (!open_) {
        captureBackdrop();  // while the window is still hidden, or we would grab ourselves
        open_ = true;
        glfwShowWindow(window_);
    }
}

void Overlay::togglePlayback() {
    if (player_) player_->toggle();
}

void Overlay::skipPlayback(double seconds) {
    if (player_) player_->skip(seconds);
}

// One wheel notch means whatever the preview on screen makes of it.
void Overlay::wheel(int notches) {
    if (player_) {
        setVolume(volume_ + static_cast<float>(notches) * kVolumeStep);
        return;
    }
    if (pdf_) pdfScroll_ -= static_cast<float>(notches) * kScrollStep;  // clamped once the layout is known
}

void Overlay::setVolume(float value) {
    volume_ = std::clamp(value, 0.0f, 1.0f);
    if (player_) player_->setVolume(volume_);
}

void Overlay::close() {
    player_.reset();
    video_ = Texture{};
    pdf_.reset();
    pdfLayout_.clear();
    pdfPages_.clear();
    open_ = false;
    anim_ = 0.0f;
    glfwHideWindow(window_);
}

// Sizes the panel to whatever it has to show, centred on the active monitor.
void Overlay::layoutWindow() {
    HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;

    const int areaWidth = info.rcWork.right - info.rcWork.left;
    const int areaHeight = info.rcWork.bottom - info.rcWork.top;
    const int maxWidth = std::min(kMaxWidth, areaWidth - 80);
    const int maxHeight = std::min(kMaxHeight, areaHeight - 80);

    int width = maxWidth;
    int height = maxHeight;

    const Entry* entry = lookup(current_);
    if (!entry) {  // nothing decoded yet, avoid a jarring resize once it lands
        width = std::min(kUnknownWidth, maxWidth);
        height = std::min(kUnknownHeight, maxHeight);
    } else if (const auto* image = entry ? std::get_if<ImageData>(&entry->preview.content) : nullptr;
        image && entry->texture.valid()) {  // an image gets a window that hugs it, never scaled up
        const float scale = std::min({1.0f, static_cast<float>(maxWidth - kChromeX) / entry->texture.width,
                                      static_cast<float>(maxHeight - kChromeY) / entry->texture.height});
        width = std::clamp(static_cast<int>(entry->texture.width * scale) + kChromeX, kMinWidth, maxWidth);
        height = std::clamp(static_cast<int>(entry->texture.height * scale) + kChromeY, kMinHeight, maxHeight);
    } else if (player_ && player_->hasVideo() && player_->width() > 0) {
        // The transport and the volume slider float over the picture, so they
        // are not part of the chrome the window has to make room for.
        const float scale = std::min({1.0f, static_cast<float>(maxWidth - kChromeX) / player_->width(),
                                      static_cast<float>(maxHeight - kChromeY) / player_->height()});
        width = std::clamp(static_cast<int>(player_->width() * scale) + kChromeX, kMinWidth, maxWidth);
        height = std::clamp(static_cast<int>(player_->height() * scale) + kChromeY, kMinHeight, maxHeight);
    } else if (pdf_ && !pdfLayout_.empty()) {
        // A page is tall, so it gets the height and asks for the width it needs.
        const pdf::Page& first = pdfLayout_.front();
        const float aspect = first.width > 0.0f ? first.height / first.width : 1.414f;
        height = maxHeight;
        width = std::clamp(static_cast<int>((maxHeight - kChromeY) / aspect) + kChromeX + 26, kMinWidth, maxWidth);
    } else if (current_.kind == Kind::Media) {
        width = std::min(kMediaWidth, maxWidth);  // audio, or video we have not opened yet
        height = std::min(kMediaHeight, maxHeight);
    } else if (entry && std::holds_alternative<InfoData>(entry->preview.content)) {
        width = std::min(kIconWidth, maxWidth);  // just an icon and a note
        height = std::min(kIconHeight, maxHeight);
    }

    if (width == windowWidth_ && height == windowHeight_) return;
    windowWidth_ = width;
    windowHeight_ = height;

    windowX_ = info.rcWork.left + (areaWidth - width) / 2;
    windowY_ = info.rcWork.top + (areaHeight - height) / 2;
    glfwSetWindowSize(window_, width, height);
    glfwSetWindowPos(window_, windowX_, windowY_);

    // The only source of rounding: corners are cut out of the window itself, so
    // there is no second radius to disagree with. CreateRoundRectRgn wants the
    // ellipse diameter, not the radius.
    HWND native = glfwGetWin32Window(window_);
    roundCorners(native);
}

// Snapshots the whole monitor, so browsing on with the arrow keys can reuse it:
// the panel simply samples the part it happens to cover.
void Overlay::captureBackdrop() {
    HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;

    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    backdropOrigin_ = ImVec2(static_cast<float>(info.rcMonitor.left), static_cast<float>(info.rcMonitor.top));
    backdropSize_ = ImVec2(static_cast<float>(width), static_cast<float>(height));

    const ImageData shot = grabScreen(info.rcMonitor.left, info.rcMonitor.top, width, height, kBlurDivisor);
    backdrop_ = shot.rgba.empty() ? Texture{} : Texture(shot);
}

// The panel draws at viewport coordinates; the snapshot is indexed in screen
// coordinates, so a control has to be translated back before it can sample it.
ImVec2 Overlay::backdropUV(const ImVec2& point) const {
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    const float screenX = static_cast<float>(windowX_) + (point.x - origin.x);
    const float screenY = static_cast<float>(windowY_) + (point.y - origin.y);
    return ImVec2((screenX - backdropOrigin_.x) / backdropSize_.x, (screenY - backdropOrigin_.y) / backdropSize_.y);
}

// A control backing cut out of the same frosted snapshot the panel sits on,
// rather than a flat plate laid over the picture.
void Overlay::drawFrosted(const ImVec2& min, const ImVec2& max, float rounding, float ease) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (backdrop_.valid())
        draw->AddImageRounded(textureId(backdrop_.id), min, max, backdropUV(min), backdropUV(max),
                              fade(IM_COL32_WHITE, ease), rounding);
    draw->AddRectFilled(min, max, fade(kScrim, ease), rounding);
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

    auto* image = std::get_if<ImageData>(&preview.content);
    if (!image) {
        if (auto* info = std::get_if<InfoData>(&preview.content)) image = &info->icon;
    }
    if (image && !image->rgba.empty()) {
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
    pumpLoader();
    pumpPdf();
    if (player_) {
        if (const auto frame = player_->takeFrame()) {
            if (!video_.valid() || video_.width != frame->width || video_.height != frame->height)
                video_ = Texture(frame->width, frame->height);
            video_.write(frame->bgra.data());
        }
    }
    layoutWindow();  // a preview that was not prefetched only reveals its size now

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

    ImDrawList* background = ImGui::GetWindowDrawList();
    if (backdrop_.valid()) {
        const ImVec2 uvMin((windowX_ - backdropOrigin_.x) / backdropSize_.x,
                           (windowY_ - backdropOrigin_.y) / backdropSize_.y);
        const ImVec2 uvMax(uvMin.x + windowWidth_ / backdropSize_.x, uvMin.y + windowHeight_ / backdropSize_.y);
        background->AddImage(textureId(backdrop_.id), pos, pos + size, uvMin, uvMax);
    }
    background->AddRectFilled(pos, pos + size, kPanelTint);

    const Entry* entry = lookup(current_);
    drawHeader(entry, drawOpenWith(pos, pos + size));

    const ImVec2 contentMin = ImGui::GetCursorScreenPos();
    const ImVec2 contentMax(pos.x + size.x - 22.0f, pos.y + size.y - 44.0f);
    drawContent(entry, contentMin, contentMax, ease);
    drawFooter(entry, contentMin, contentMax, ease);

    ImGui::End();
    ImGui::PopStyleVar(2);
}

// Top right: hands the file to whatever application owns its type. Returns the
// x the title has to stop at, so a long name never runs into the button.
float Overlay::drawOpenWith(const ImVec2& panelMin, const ImVec2& panelMax) {
    const float edge = panelMax.x - 22.0f;
    if (current_.opensWith.empty()) return edge;

    ImFont* font = fonts_.ui ? fonts_.ui : ImGui::GetFont();
    const float textSize = font->FontSize;
    const std::string label = "Open with " + current_.opensWith;
    const float width = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, label.c_str()).x + 26.0f;
    const float height = textSize + 14.0f;

    const ImVec2 min(edge - width, panelMin.y + 12.0f);
    const ImVec2 max(min.x + width, min.y + height);

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##openwith", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        ShellExecuteW(nullptr, L"open", current_.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        close();
    }
    ImGui::SetCursorScreenPos(cursor);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, hovered ? kButtonHover : kButton, kButtonRounding);
    draw->AddText(font, textSize, ImVec2(min.x + 13.0f, min.y + (height - textSize) * 0.5f), kText, label.c_str());

    return min.x - 16.0f;
}

void Overlay::drawHeader(const Entry* entry, float titleLimit) {
    if (fonts_.ui) ImGui::PushFont(fonts_.ui);
    ImGui::TextUnformatted(
        ellipsize(current_.name, ImGui::GetFont(), ImGui::GetFontSize(), titleLimit - ImGui::GetCursorScreenPos().x)
            .c_str());
    if (fonts_.ui) ImGui::PopFont();

    std::string meta = std::format("{}   {}", current_.ext.empty() ? "file" : current_.ext, humanSize(current_.size));
    if (player_) {
        if (const std::string details = player_->details(); !details.empty()) meta += "   " + details;
        meta += std::format("   {}", clockText(player_->duration()));
    } else if (pdf_ && pdf_->pageCount() > 0) {
        meta += std::format("   {} pages", pdf_->pageCount());
    } else if (entry) {
        if (const auto* image = std::get_if<ImageData>(&entry->preview.content))
            meta += std::format("   {} x {} px", image->sourceWidth, image->sourceHeight);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(meta.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// Video picture or cover art. The controls float on top of it: the picture is
// given the whole content area and pays nothing for them.
void Overlay::drawMedia(const Entry* entry, const ImVec2& min, const ImVec2& max, float ease) {
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (player_->failed()) {
        centeredText(ImGui::GetFont(), 16.0f, min, max, (min.y + max.y) * 0.5f, fade(kMuted, ease),
                     "Windows cannot decode this file");
        return;
    }

    if (video_.valid()) {
        const auto [pos, size] = fitInto(min, max, video_.width, video_.height);
        draw->AddImageRounded(textureId(video_.id), pos, pos + size, ImVec2(0, 0), ImVec2(1, 1),
                              fade(IM_COL32_WHITE, ease), 8.0f);
    } else if (entry && entry->texture.valid()) {  // cover art, or the poster the shell gave us
        const float side = std::min({256.0f, max.x - min.x, max.y - min.y});
        const float scale = side / static_cast<float>(std::max(entry->texture.width, entry->texture.height));
        const ImVec2 centre((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        const ImVec2 half(entry->texture.width * scale * 0.5f, entry->texture.height * scale * 0.5f);
        draw->AddImage(textureId(entry->texture.id), centre - half, centre + half, ImVec2(0, 0), ImVec2(1, 1),
                       fade(IM_COL32_WHITE, ease));
    }

    if (player_->hasAudio()) drawVolume(min, max, ease);
    drawTransport(min, max, ease);
}

void Overlay::drawTransport(const ImVec2& min, const ImVec2& max, float ease) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();

    const float stripTop = max.y - kTimelineHeight;
    const double duration = player_->duration();
    const double position = duration > 0.0 ? std::min(player_->position(), duration) : player_->position();

    drawFrosted(ImVec2(min.x, stripTop), max, 10.0f, ease);  // it sits over the picture, so it brings its own backing

    // --- seek bar: the whole strip is the hit area, not just the 6 px it draws
    const ImVec2 trackMin(min.x + 14.0f, stripTop + 16.0f);
    const ImVec2 trackMax(max.x - 14.0f, trackMin.y + 6.0f);
    const Hit bar = hitTest("##seek", ImVec2(trackMin.x, trackMin.y - 11.0f), ImVec2(trackMax.x, trackMax.y + 11.0f));
    if (bar.active && duration > 0.0) {
        const float reached = (ImGui::GetIO().MousePos.x - trackMin.x) / (trackMax.x - trackMin.x);
        player_->seek(std::clamp(reached, 0.0f, 1.0f) * duration);
    }

    const float grow = bar.hovered ? 2.0f : 0.0f;  // thickens under the cursor, so it reads as clickable
    const ImVec2 barMin(trackMin.x, trackMin.y - grow);
    const ImVec2 barMax(trackMax.x, trackMax.y + grow);
    draw->AddRectFilled(barMin, barMax, fade(kTrack, ease), 3.0f);
    if (duration > 0.0) {
        const auto played = static_cast<float>(position / duration);
        const float head = trackMin.x + (trackMax.x - trackMin.x) * played;
        draw->AddRectFilled(barMin, ImVec2(head, barMax.y), fade(kAccent, ease), 3.0f);
        if (bar.hovered) draw->AddCircleFilled(ImVec2(head, (barMin.y + barMax.y) * 0.5f), 7.0f, fade(kText, ease));
    }

    // --- transport buttons, centred under the bar
    const float row = stripTop + 56.0f;
    const float side = 34.0f;
    const float centreX = (min.x + max.x) * 0.5f;
    const float step = side + 12.0f;

    struct Button {
        const char* id;
        float offset;
        int glyph;  // -1 back, 0 play/pause, +1 forward
    };
    for (const Button& button : {Button{"##back", -step, -1}, Button{"##play", 0.0f, 0}, Button{"##fwd", step, 1}}) {
        const ImVec2 centre(centreX + button.offset, row);
        const ImVec2 buttonMin(centre.x - side * 0.5f, centre.y - side * 0.5f);
        const ImVec2 buttonMax(centre.x + side * 0.5f, centre.y + side * 0.5f);
        const Hit state = hitTest(button.id, buttonMin, buttonMax);

        if (state.clicked) {
            if (button.glyph == 0)
                player_->toggle();
            else
                player_->skip(button.glyph * kSkipSeconds);
        }

        draw->AddRectFilled(buttonMin, buttonMax, fade(state.hovered ? kButtonHover : kButton, ease), kButtonRounding);
        const ImU32 ink = fade(kText, ease);
        if (button.glyph < 0)
            glyphSkip(draw, centre, -1.0f, ink);
        else if (button.glyph > 0)
            glyphSkip(draw, centre, 1.0f, ink);
        else if (player_->playing())
            glyphPause(draw, centre, ink);
        else
            glyphPlay(draw, centre, ink);
    }

    // --- clock on the left, state on the right, both clear of the buttons
    const float textY = row - 8.0f;
    draw->AddText(font, 15.0f, ImVec2(min.x + 16.0f, textY), fade(kMuted, ease),
                  std::format("{} / {}", clockText(position), clockText(duration)).c_str());

    const char* state = player_->finished() ? "ended" : (player_->playing() ? "playing" : "paused");
    const float width = font->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, state).x;
    draw->AddText(font, 15.0f, ImVec2(max.x - 16.0f - width, textY), fade(kMuted, ease), state);
}

// Vertical slider floating over the right hand edge of the picture, level shown
// underneath. Dragging it and the mouse wheel set the same value.
void Overlay::drawVolume(const ImVec2& min, const ImVec2& max, float ease) {
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const float centreX = max.x - kVolumeColumn * 0.5f;
    const float available = max.y - kTimelineHeight - min.y;  // clear of the transport below
    const float span = std::clamp(available * 0.5f, 48.0f, 150.0f);
    const float top = min.y + (available - span) * 0.5f;
    const float bottom = top + span;

    const Hit slider =
        hitTest("##volume", ImVec2(centreX - 15.0f, top - 12.0f), ImVec2(centreX + 15.0f, bottom + 12.0f));
    if (slider.active) setVolume(1.0f - (ImGui::GetIO().MousePos.y - top) / span);

    drawFrosted(ImVec2(centreX - 15.0f, top - 14.0f), ImVec2(centreX + 15.0f, bottom + 30.0f), 15.0f, ease);

    draw->AddRectFilled(ImVec2(centreX - 2.5f, top), ImVec2(centreX + 2.5f, bottom), fade(kTrack, ease), 2.5f);
    const float level = bottom - span * volume_;
    draw->AddRectFilled(ImVec2(centreX - 2.5f, level), ImVec2(centreX + 2.5f, bottom), fade(kAccent, ease), 2.5f);
    draw->AddCircleFilled(ImVec2(centreX, level), slider.hovered || slider.active ? 7.0f : 5.5f, fade(kText, ease));

    centeredText(ImGui::GetFont(), 13.0f, ImVec2(centreX - 20.0f, 0.0f), ImVec2(centreX + 20.0f, 0.0f), bottom + 11.0f,
                 fade(kMuted, ease), std::format("{}%", static_cast<int>(volume_ * 100.0f + 0.5f)));
}

// Rasterised pages arrive from the worker; the render thread is the only one
// that may touch OpenGL, so this is where they become textures.
void Overlay::pumpPdf() {
    if (!pdf_) return;
    if (pdfLayout_.empty() && pdf_->ready()) pdfLayout_ = pdf_->pages();

    for (auto& rendered : pdf_->collect()) {
        if (rendered.image.rgba.empty()) continue;
        pdfPages_.insert_or_assign(rendered.index, Texture(rendered.image));
    }
}

// The whole document as one scrolling column. Page sizes are known up front, so
// the column can be laid out and scrolled before anything has been rasterised;
// only the pages the viewport actually reaches are ever asked for.
void Overlay::drawPdf(const ImVec2& min, const ImVec2& max, float ease) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();

    if (pdf_->failed()) {
        centeredText(font, 16.0f, min, max, (min.y + max.y) * 0.5f, fade(kMuted, ease),
                     "Windows cannot open this PDF");
        return;
    }
    if (pdfLayout_.empty()) {
        centeredText(font, 16.0f, min, max, (min.y + max.y) * 0.5f, fade(kMuted, ease), "opening...");
        return;
    }

    const float viewport = max.y - min.y;
    const float pageWidth = std::max(32.0f, max.x - min.x - 26.0f);  // leaves the scrollbar its lane

    // Lay the column out, then clamp whatever the wheel did to it.
    std::vector<float> tops;
    tops.reserve(pdfLayout_.size());
    float cursor = 0.0f;
    for (const pdf::Page& page : pdfLayout_) {
        tops.push_back(cursor);
        const float aspect = page.width > 0.0f ? page.height / page.width : 1.414f;
        cursor += pageWidth * aspect + kPageGap;
    }
    const float total = std::max(0.0f, cursor - kPageGap);
    pdfScroll_ = std::clamp(pdfScroll_, 0.0f, std::max(0.0f, total - viewport));

    const float left = min.x + (max.x - min.x - pageWidth - 18.0f) * 0.5f;
    int firstVisible = -1;
    int lastVisible = -1;

    // A page scrolled halfway out would otherwise paint straight over the title
    // and the key hints: the column is only ever allowed inside the content area.
    draw->PushClipRect(min, max, true);

    for (int index = 0; index < static_cast<int>(pdfLayout_.size()); ++index) {
        const pdf::Page& page = pdfLayout_[index];
        const float aspect = page.width > 0.0f ? page.height / page.width : 1.414f;
        const float height = pageWidth * aspect;
        const float top = min.y + tops[index] - pdfScroll_;
        if (top > max.y || top + height < min.y) continue;

        if (firstVisible < 0) firstVisible = index;
        lastVisible = index;

        const ImVec2 pageMin(left, top);
        const ImVec2 pageMax(left + pageWidth, top + height);

        const auto it = pdfPages_.find(index);
        if (it != pdfPages_.end() && it->second.valid()) {
            draw->AddImageRounded(textureId(it->second.id), pageMin, pageMax, ImVec2(0, 0), ImVec2(1, 1),
                                  fade(IM_COL32_WHITE, ease), 4.0f);
        } else {  // a blank sheet holds the place, so the column never jumps
            pdf_->request(index, static_cast<int>(pageWidth));
            draw->AddRectFilled(pageMin, pageMax, fade(IM_COL32(236, 238, 243, 255), ease * 0.5f), 4.0f);
            centeredText(font, 15.0f, pageMin, pageMax, (pageMin.y + pageMax.y) * 0.5f, fade(kMuted, ease),
                         std::format("page {}", index + 1));
        }
    }

    draw->PopClipRect();

    if (firstVisible >= 0) pdfPage_ = firstVisible;

    // Pages far from the viewport give their texture back.
    if (firstVisible >= 0) {
        std::erase_if(pdfPages_, [&](const auto& entry) {
            return entry.first < firstVisible - kPagesAround || entry.first > lastVisible + kPagesAround;
        });
    }

    // --- scrollbar, only worth drawing when there is something to scroll
    if (total > viewport) {
        const float trackX = max.x - 5.0f;
        const float fraction = viewport / total;
        const float thumb = std::max(28.0f, viewport * fraction);
        const float travel = viewport - thumb;
        const float offset = total > viewport ? pdfScroll_ / (total - viewport) : 0.0f;
        draw->AddRectFilled(ImVec2(trackX - 2.5f, min.y), ImVec2(trackX + 2.5f, max.y), fade(kTrack, ease * 0.6f),
                            2.5f);
        draw->AddRectFilled(ImVec2(trackX - 2.5f, min.y + travel * offset),
                            ImVec2(trackX + 2.5f, min.y + travel * offset + thumb), fade(kAccent, ease), 2.5f);
    }

    // --- page indicator
    if (pdfLayout_.size() > 1) {
        const std::string label = std::format("{} / {}", pdfPage_ + 1, pdfLayout_.size());
        const float width = font->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, label.c_str()).x;
        const ImVec2 pillMin((min.x + max.x - width) * 0.5f - 14.0f, max.y - 40.0f);
        const ImVec2 pillMax(pillMin.x + width + 28.0f, pillMin.y + 30.0f);
        drawFrosted(pillMin, pillMax, 15.0f, ease);
        draw->AddText(font, 15.0f, ImVec2(pillMin.x + 14.0f, pillMin.y + 7.0f), fade(kText, ease), label.c_str());
    }
}

void Overlay::drawContent(const Entry* entry, const ImVec2& min, const ImVec2& max, float ease) {
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (player_) {
        drawMedia(entry, min, max, ease);
        return;
    }

    if (pdf_) {
        drawPdf(min, max, ease);
        return;
    }

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

    const ImVec2 centre((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f - 40.0f);
    float noteY = 0.0f;

    if (entry->texture.valid()) {  // whatever Explorer shows for this file
        const float side = std::min({256.0f, (max.x - min.x) * 0.6f, (max.y - min.y) * 0.5f});
        const float scale = side / static_cast<float>(std::max(entry->texture.width, entry->texture.height));
        const ImVec2 half(entry->texture.width * scale * 0.5f, entry->texture.height * scale * 0.5f);
        draw->AddImage(textureId(entry->texture.id), centre - half, centre + half, ImVec2(0, 0), ImVec2(1, 1),
                       fade(IM_COL32_WHITE, ease));
        noteY = centre.y + half.y + 28.0f;
    } else {
        const ImVec2 badgeMin(centre.x - 60.0f, centre.y - 60.0f);
        const ImVec2 badgeMax(centre.x + 60.0f, centre.y + 60.0f);
        draw->AddRectFilled(badgeMin, badgeMax, fade(kindColor(current_.kind), ease), 12.0f);
        centeredText(ImGui::GetFont(), 24.0f, badgeMin, badgeMax, badgeMin.y + 46.0f, fade(kText, ease),
                     current_.ext.empty() ? "FILE" : current_.ext);
        noteY = badgeMax.y + 24.0f;
    }

    const auto* info = std::get_if<InfoData>(&entry->preview.content);
    centeredText(ImGui::GetFont(), 17.0f, min, max, noteY, fade(kMuted, ease),
                 info ? info->note : std::string{"No preview available"});
}

void Overlay::drawFooter(const Entry* entry, const ImVec2& min, const ImVec2& max, float ease) {
    const auto* text = entry ? std::get_if<TextData>(&entry->preview.content) : nullptr;

    std::string footer = "Space / Esc close     arrows browse";
    if (player_) footer += "     Enter play / pause     wheel volume";
    if (pdf_) footer += "     wheel scrolls";
    if (text && text->truncated) footer += "     truncated at 1 MiB";

    centeredText(ImGui::GetFont(), 16.0f, min, max, max.y + 6.0f, fade(kMuted, ease), footer);
}

}  // namespace pb
