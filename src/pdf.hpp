#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "preview.hpp"  // ImageData

// PDF rendering through Windows.Data.Pdf, which is part of Windows just like
// Media Foundation is - no third party renderer, nothing to ship alongside.
namespace pb::pdf {

struct Page {
    float width = 0.0f;  // the document's own units: enough to lay it out before anything is rendered
    float height = 0.0f;
};

struct Rendered {
    int index = 0;
    ImageData image;
};

// One open document. A worker thread owns the WinRT objects and rasterises pages
// on request; the render thread only ever asks and collects, the same division
// of labour as media::Player.
class Document {
public:
    explicit Document(std::filesystem::path file);
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_relaxed); }
    [[nodiscard]] int pageCount() const noexcept { return pageCount_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::vector<Page> pages() const;

    // Queues one page at the given pixel width. A page already queued or being
    // rasterised is ignored, so calling this every frame costs nothing.
    void request(int index, int pixelWidth);
    [[nodiscard]] std::vector<Rendered> collect();

private:
    void run(std::stop_token stop);

    std::filesystem::path path_;

    mutable std::mutex mutex_;
    std::vector<Page> pages_;
    std::deque<std::pair<int, int>> queue_;  // page index, pixel width
    std::unordered_set<int> claimed_;        // queued or in flight
    std::vector<Rendered> done_;

    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::atomic<int> pageCount_{0};

    std::jthread worker_;  // declared last: joined before the state above dies
};

}  // namespace pb::pdf
