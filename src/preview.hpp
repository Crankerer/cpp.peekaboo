#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>
#include <variant>
#include <vector>

namespace pb {

enum class Kind { Image, Text, Media, Pdf, Other };

struct FileEntry {
    std::filesystem::path path;
    std::string name;  // UTF-8, for display
    std::string ext;        // lower case, without dot
    std::string opensWith;  // friendly name of the application registered for this type
    std::uintmax_t size{};
    Kind kind{Kind::Other};
};

[[nodiscard]] FileEntry describe(const std::filesystem::path& path);
[[nodiscard]] std::vector<FileEntry> scanDirectory(const std::filesystem::path& dir);
[[nodiscard]] std::string humanSize(std::uintmax_t bytes);
[[nodiscard]] std::string toUtf8(const std::filesystem::path& p);

struct ImageData {
    int width{};
    int height{};
    int sourceWidth{};   // resolution before the preview was downscaled
    int sourceHeight{};
    std::vector<std::uint8_t> rgba;  // emptied once uploaded to the GPU
};

// Decodes an image that is already in memory, through Windows' own codecs.
// Empty if Windows cannot read it. Callers must have COM running on the thread.
[[nodiscard]] ImageData decodeEncodedImage(const void* data, std::size_t size);

struct TextData {
    std::string text;
    std::vector<std::uint32_t> lines;  // start offset of every line
    bool truncated{};
};

struct InfoData {  // fallback for everything we cannot render
    std::string note;
    ImageData icon;  // whatever the shell shows for this file, empty if it has nothing
};

using Content = std::variant<ImageData, TextData, InfoData>;

struct Preview {
    std::filesystem::path path;
    Content content;
    std::size_t bytes{};  // approximate footprint, drives the cache budget
    double loadMs{};
};

// Decodes previews on worker threads. Nothing here ever runs on the render
// thread: requests are queued, finished previews are picked up via collect().
class PreviewLoader {
public:
    explicit PreviewLoader(unsigned workers = 0);
    ~PreviewLoader();
    PreviewLoader(const PreviewLoader&) = delete;
    PreviewLoader& operator=(const PreviewLoader&) = delete;

    // Urgent requests jump the queue; paths already queued or in flight are ignored.
    void request(const FileEntry& file, bool urgent);
    [[nodiscard]] std::vector<Preview> collect();
    [[nodiscard]] std::size_t pending() const;
    [[nodiscard]] unsigned workerCount() const noexcept { return static_cast<unsigned>(workers_.size()); }

private:
    void run(std::stop_token stop);

    mutable std::mutex mutex_;
    std::condition_variable_any wakeup_;
    std::deque<FileEntry> queue_;
    std::unordered_set<std::filesystem::path::string_type> known_;  // queued or in flight
    std::vector<Preview> done_;
    std::vector<std::jthread> workers_;  // declared last: joined before the state above dies
};

}  // namespace pb
