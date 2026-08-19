#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pb::media {

// Once per process, on the thread that runs the message loop.
bool initialize();
void shutdown();

struct Frame {
    int width = 0;
    int height = 0;
    double timestamp = 0.0;          // seconds into the file
    std::vector<std::uint8_t> bgra;  // the byte order OpenGL takes directly
};

// Plays one media file. A worker thread demuxes and decodes through Media
// Foundation, the audio device drains a ring buffer and doubles as the clock,
// and the render thread only ever asks which frame is due now.
class Player {
public:
    explicit Player(std::filesystem::path file);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void toggle();

    [[nodiscard]] bool playing() const noexcept { return playing_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool finished() const noexcept { return finished_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool hasVideo() const noexcept { return hasVideo_.load(std::memory_order_relaxed); }
    [[nodiscard]] int width() const noexcept { return width_.load(std::memory_order_relaxed); }
    [[nodiscard]] int height() const noexcept { return height_.load(std::memory_order_relaxed); }
    [[nodiscard]] double duration() const noexcept { return duration_.load(std::memory_order_relaxed); }
    [[nodiscard]] double position() const;
    [[nodiscard]] std::string details() const;

    // The frame due now, or nothing if the one already on screen still holds.
    [[nodiscard]] std::optional<Frame> takeFrame();

    // Called by the audio device; public only because it crosses a C callback.
    void fillAudio(std::int16_t* output, std::size_t frames);

private:
    void run(std::stop_token stop);
    void pushAudio(const std::int16_t* samples, std::size_t count, const std::stop_token& stop);
    void pushFrame(Frame&& frame, const std::stop_token& stop);

    std::filesystem::path path_;

    mutable std::mutex mutex_;
    std::vector<std::int16_t> ring_;
    std::size_t ringRead_ = 0;
    std::size_t ringWrite_ = 0;
    std::size_t ringFill_ = 0;
    std::deque<Frame> frames_;
    std::string details_;

    std::atomic<bool> playing_{true};
    std::atomic<bool> finished_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> hasVideo_{false};
    std::atomic<bool> hasAudio_{false};
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    std::atomic<double> duration_{0.0};
    std::atomic<std::uint64_t> playedFrames_{0};  // audio frames handed to the device: our clock

    double silentPosition_ = 0.0;  // clock for files without an audio track
    std::chrono::steady_clock::time_point resumedAt_{};

    std::jthread worker_;  // declared last: joined before the state above dies
};

}  // namespace pb::media
