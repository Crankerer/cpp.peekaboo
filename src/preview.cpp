#include "preview.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <limits>
#include <memory>
#include <span>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#define STBI_THREAD_LOCAL thread_local
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

namespace pb {
namespace {

constexpr std::size_t kTextLimit = 1u << 20;  // read at most 1 MiB of text
constexpr std::size_t kSniffLimit = 4096;
constexpr std::size_t kBlobLimit = 64u << 20;
constexpr int kMaxImageDim = 2048;  // decoded previews are capped to keep GPU uploads cheap
constexpr std::size_t kMaxQueued = 24;

// --- memory mapped file: the only place we touch the raw OS file APIs -------
class MappedFile {
public:
    explicit MappedFile(const fs::path& path);
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

private:
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

#ifdef _WIN32
MappedFile::MappedFile(const fs::path& path) {
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size) || size.QuadPart <= 0) return;

    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) return;

    data_ = static_cast<const std::byte*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (data_) size_ = static_cast<std::size_t>(size.QuadPart);
}

MappedFile::~MappedFile() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}
#else
MappedFile::MappedFile(const fs::path& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return;

    struct stat info{};
    if (::fstat(fd_, &info) != 0 || info.st_size <= 0) return;

    void* view = ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd_, 0);
    if (view == MAP_FAILED) return;

    data_ = static_cast<const std::byte*>(view);
    size_ = static_cast<std::size_t>(info.st_size);
}

MappedFile::~MappedFile() {
    if (data_) ::munmap(const_cast<std::byte*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
}
#endif

// --- classification ---------------------------------------------------------
std::string lowerAscii(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Kind kindForExtension(std::string_view ext) {
    static constexpr std::array images{"png", "jpg", "jpeg", "bmp", "gif", "webp", "tga", "psd", "hdr", "ppm", "pgm"};
    static constexpr std::array texts{
        "txt",  "md",   "markdown", "cpp",  "cxx",  "cc",   "c",     "h",    "hpp",  "hxx", "inl", "cs",
        "java", "kt",   "py",       "rb",   "rs",   "go",   "js",    "ts",   "jsx",  "tsx", "json", "xml",
        "html", "htm",  "css",      "scss", "yaml", "yml",  "toml",  "ini",  "cfg",  "conf", "log", "csv",
        "tsv",  "sql",  "sh",       "bat",  "ps1",  "cmake", "make", "glsl", "hlsl", "lua", "php",  "swift"};

    const auto matches = [ext](const auto& list) { return std::ranges::find(list, ext) != list.end(); };
    if (matches(images)) return Kind::Image;
    if (matches(texts)) return Kind::Text;
    return Kind::Other;
}

bool looksLikeText(std::span<const std::byte> raw) {
    const std::size_t n = std::min(raw.size(), kSniffLimit);
    std::size_t suspicious = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = static_cast<unsigned char>(raw[i]);
        if (c == 0) return false;
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ++suspicious;
    }
    return suspicious * 20 < n;  // tolerate a sprinkling of control bytes
}

// --- decoding ---------------------------------------------------------------
ImageData boxDownscale(const std::uint8_t* src, int width, int height, int factor) {
    ImageData out;
    out.width = width / factor;
    out.height = height / factor;
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);

    const int samples = factor * factor;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            std::array<int, 4> sum{};
            for (int dy = 0; dy < factor; ++dy) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y * factor + dy) * width + static_cast<std::size_t>(x) * factor) * 4;
                const std::uint8_t* row = src + offset;
                for (int dx = 0; dx < factor; ++dx)
                    for (int c = 0; c < 4; ++c) sum[c] += row[dx * 4 + c];
            }
            std::uint8_t* dst = out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4;
            for (int c = 0; c < 4; ++c) dst[c] = static_cast<std::uint8_t>(sum[c] / samples);
        }
    }
    return out;
}

Content decodeImage(std::span<const std::byte> raw) {
    int width = 0;
    int height = 0;
    int channels = 0;
    const std::unique_ptr<stbi_uc, decltype([](stbi_uc* p) { stbi_image_free(p); })> pixels{stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(raw.data()), static_cast<int>(raw.size()), &width, &height, &channels, 4)};

    if (!pixels) return InfoData{std::format("Cannot decode image ({})", stbi_failure_reason())};

    const int factor = (std::max(width, height) + kMaxImageDim - 1) / kMaxImageDim;
    if (factor > 1) return boxDownscale(pixels.get(), width, height, factor);

    ImageData image;
    image.width = width;
    image.height = height;
    image.rgba.assign(pixels.get(), pixels.get() + static_cast<std::size_t>(width) * height * 4);
    return image;
}

Content decodeText(std::span<const std::byte> raw) {
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF)
        raw = raw.subspan(3);

    TextData text;
    const std::size_t take = std::min(raw.size(), kTextLimit);
    text.truncated = take < raw.size();
    text.text.reserve(take);

    for (std::size_t i = 0; i < take; ++i) {
        const char c = static_cast<char>(raw[i]);
        if (c == '\r') continue;
        if (c == '\t') {
            text.text.append(4, ' ');
            continue;
        }
        text.text.push_back(c);
    }

    text.lines.push_back(0);
    for (std::size_t i = 0; i < text.text.size(); ++i)
        if (text.text[i] == '\n') text.lines.push_back(static_cast<std::uint32_t>(i + 1));

    return text;
}

Content loadContent(const FileEntry& file) {
    if (file.size == 0) return InfoData{"Empty file"};
    if (file.ext == "webp") return InfoData{"WebP is not supported by stb_image"};
    if (file.kind == Kind::Other && file.size > kBlobLimit) return InfoData{"No preview available"};

    const MappedFile mapped(file.path);
    if (!mapped.valid()) return InfoData{"File could not be opened"};

    const auto raw = mapped.bytes();
    if (raw.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return InfoData{"File too large to preview"};

    if (file.kind == Kind::Image) return decodeImage(raw);
    if (file.kind == Kind::Text || looksLikeText(raw)) return decodeText(raw);
    return InfoData{"No preview available"};
}

std::size_t footprint(const Content& content) {
    if (const auto* image = std::get_if<ImageData>(&content))
        return static_cast<std::size_t>(image->width) * image->height * 4;
    if (const auto* text = std::get_if<TextData>(&content)) return text->text.size() + text->lines.size() * 4;
    return 256;
}

Preview decode(const FileEntry& file) {
    const auto started = std::chrono::steady_clock::now();
    Preview preview;
    preview.path = file.path;
    preview.content = loadContent(file);
    preview.bytes = footprint(preview.content);
    preview.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return preview;
}

}  // namespace

// --- helpers ----------------------------------------------------------------
std::string toUtf8(const fs::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string humanSize(std::uintmax_t bytes) {
    static constexpr std::array units{"B", "KB", "MB", "GB", "TB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", value, units[unit]);
}

std::vector<FileEntry> scanDirectory(const fs::path& dir) {
    std::vector<FileEntry> files;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file(ec)) continue;

        FileEntry file;
        file.path = entry.path();
        file.name = toUtf8(entry.path().filename());
        file.ext = lowerAscii(toUtf8(entry.path().extension()));
        if (!file.ext.empty()) file.ext.erase(0, 1);
        file.size = entry.file_size(ec);
        file.kind = kindForExtension(file.ext);
        files.push_back(std::move(file));
    }

    std::ranges::sort(files, [](const FileEntry& a, const FileEntry& b) { return lowerAscii(a.name) < lowerAscii(b.name); });
    return files;
}

#ifdef _WIN32
std::optional<fs::path> pickFolder() {
    const HRESULT started = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::optional<fs::path> chosen;
    {  // every COM object must be released before CoUninitialize
        IFileOpenDialog* rawDialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rawDialog)))) {
            const std::unique_ptr<IFileOpenDialog, decltype([](IFileOpenDialog* p) { p->Release(); })> dialog{rawDialog};
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

            IShellItem* rawItem = nullptr;
            if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&rawItem))) {
                const std::unique_ptr<IShellItem, decltype([](IShellItem* p) { p->Release(); })> item{rawItem};
                PWSTR name = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                    chosen = fs::path(name);
                    CoTaskMemFree(name);
                }
            }
        }
    }
    if (SUCCEEDED(started)) CoUninitialize();
    return chosen;
}
#else
std::optional<fs::path> pickFolder() { return std::nullopt; }
#endif

// --- loader -----------------------------------------------------------------
PreviewLoader::PreviewLoader(unsigned workers) {
    if (workers == 0) workers = std::max(2u, std::thread::hardware_concurrency() - 1);
    workers_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i)
        workers_.emplace_back([this](std::stop_token stop) { run(std::move(stop)); });
}

PreviewLoader::~PreviewLoader() = default;

void PreviewLoader::request(const FileEntry& file, bool urgent) {
    {
        const std::lock_guard lock(mutex_);
        if (!known_.insert(file.path.native()).second) return;

        if (urgent)
            queue_.push_front(file);
        else
            queue_.push_back(file);

        while (queue_.size() > kMaxQueued) {  // stale prefetches are worthless, drop them
            known_.erase(queue_.back().path.native());
            queue_.pop_back();
        }
    }
    wakeup_.notify_one();
}

std::vector<Preview> PreviewLoader::collect() {
    const std::lock_guard lock(mutex_);
    auto finished = std::move(done_);
    done_.clear();
    for (const auto& preview : finished) known_.erase(preview.path.native());
    return finished;
}

std::size_t PreviewLoader::pending() const {
    const std::lock_guard lock(mutex_);
    return known_.size();
}

void PreviewLoader::run(std::stop_token stop) {
    while (true) {
        FileEntry job;
        {
            std::unique_lock lock(mutex_);
            wakeup_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (stop.stop_requested()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        Preview preview = decode(job);
        const std::lock_guard lock(mutex_);
        done_.push_back(std::move(preview));
    }
}

}  // namespace pb
