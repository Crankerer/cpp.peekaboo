#include "media.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>

namespace fs = std::filesystem;

namespace pb::media {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr std::size_t kRingSamples = kSampleRate * kChannels * 3;  // three seconds of slack
constexpr std::size_t kMaxFrames = 6;
constexpr int kMaxVideoWidth = 1366;  // no point decoding larger than the panel can show
constexpr int kMaxVideoHeight = 768;
constexpr DWORD kNoStream = 0xFFFFFFFF;

template <class T>
struct Release {
    void operator()(T* p) const noexcept { p->Release(); }
};
template <class T>
using ComPtr = std::unique_ptr<T, Release<T>>;

template <class T, class Call>
ComPtr<T> query(Call&& call) {
    T* raw = nullptr;
    return SUCCEEDED(call(&raw)) ? ComPtr<T>{raw} : ComPtr<T>{};
}

struct Source {
    ComPtr<IMFSourceReader> reader;
    DWORD video = kNoStream;
    DWORD audio = kNoStream;
    int width = 0;
    int height = 0;
    int stride = 0;  // negative means the rows arrive bottom-up
    double duration = 0.0;
    std::string details;
};

void configureVideo(Source& source) {
    IMFSourceReader* reader = source.reader.get();
    reader->SetStreamSelection(source.video, TRUE);

    UINT32 nativeWidth = 0;
    UINT32 nativeHeight = 0;
    if (const auto native =
            query<IMFMediaType>([&](IMFMediaType** out) { return reader->GetNativeMediaType(source.video, 0, out); }))
        MFGetAttributeSize(native.get(), MF_MT_FRAME_SIZE, &nativeWidth, &nativeHeight);

    const auto type = query<IMFMediaType>([](IMFMediaType** out) { return MFCreateMediaType(out); });
    if (!type) {
        source.video = kNoStream;
        return;
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

    // Ask the video processor to scale for us; far cheaper than moving full size
    // frames around just to shrink them at draw time.
    if (nativeWidth > 0 && nativeHeight > 0) {
        const double scale = std::min({1.0, static_cast<double>(kMaxVideoWidth) / nativeWidth,
                                       static_cast<double>(kMaxVideoHeight) / nativeHeight});
        if (scale < 1.0) {
            const auto width = static_cast<UINT32>(nativeWidth * scale) & ~1u;
            const auto height = static_cast<UINT32>(nativeHeight * scale) & ~1u;
            MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, width, height);
        }
    }

    if (FAILED(reader->SetCurrentMediaType(source.video, nullptr, type.get()))) {
        type->DeleteItem(MF_MT_FRAME_SIZE);  // some sources refuse to scale, take them as they are
        if (FAILED(reader->SetCurrentMediaType(source.video, nullptr, type.get()))) {
            source.video = kNoStream;
            return;
        }
    }

    const auto actual =
        query<IMFMediaType>([&](IMFMediaType** out) { return reader->GetCurrentMediaType(source.video, out); });
    if (!actual) {
        source.video = kNoStream;
        return;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    MFGetAttributeSize(actual.get(), MF_MT_FRAME_SIZE, &width, &height);
    source.width = static_cast<int>(width);
    source.height = static_cast<int>(height);

    LONG stride = 0;
    if (FAILED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride))))
        MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride);
    source.stride = static_cast<int>(stride);
}

void configureAudio(Source& source) {
    IMFSourceReader* reader = source.reader.get();
    reader->SetStreamSelection(source.audio, TRUE);

    const auto type = query<IMFMediaType>([](IMFMediaType** out) { return MFCreateMediaType(out); });
    if (!type) {
        source.audio = kNoStream;
        return;
    }

    // Pin one format so the output device never has to be reconfigured.
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * kChannels * 2);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    if (FAILED(reader->SetCurrentMediaType(source.audio, nullptr, type.get()))) source.audio = kNoStream;
}

Source openSource(const fs::path& path) {
    Source source;

    auto attributes = query<IMFAttributes>([](IMFAttributes** out) { return MFCreateAttributes(out, 1); });
    if (attributes) attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    IMFSourceReader* raw = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), &raw))) return source;
    source.reader.reset(raw);

    source.reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    for (DWORD index = 0;; ++index) {
        const auto native = query<IMFMediaType>(
            [&](IMFMediaType** out) { return source.reader->GetNativeMediaType(index, 0, out); });
        if (!native) break;

        GUID major{};
        if (FAILED(native->GetGUID(MF_MT_MAJOR_TYPE, &major))) continue;
        if (major == MFMediaType_Video && source.video == kNoStream) source.video = index;
        if (major == MFMediaType_Audio && source.audio == kNoStream) source.audio = index;
    }

    if (source.video != kNoStream) configureVideo(source);
    if (source.audio != kNoStream) configureAudio(source);

    PROPVARIANT duration{};
    if (SUCCEEDED(source.reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)))
        source.duration = static_cast<double>(duration.uhVal.QuadPart) / 1e7;
    PropVariantClear(&duration);

    if (source.video != kNoStream) source.details = std::format("{} x {} px", source.width, source.height);
    if (source.audio != kNoStream) {
        if (!source.details.empty()) source.details += "   ";
        source.details += std::format("{} kHz stereo", kSampleRate / 1000);
    }
    return source;
}

void audioCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    static_cast<Player*>(device->pUserData)->fillAudio(static_cast<std::int16_t*>(output), frames);
}

}  // namespace

bool initialize() { return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }

void shutdown() { MFShutdown(); }

Player::Player(fs::path file) : path_(std::move(file)), ring_(kRingSamples) {
    resumedAt_ = std::chrono::steady_clock::now();
    worker_ = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
}

Player::~Player() = default;

void Player::toggle() {
    const std::lock_guard lock(mutex_);
    const bool wasPlaying = playing_.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();

    if (wasPlaying)
        silentPosition_ += std::chrono::duration<double>(now - resumedAt_).count();
    else
        resumedAt_ = now;

    playing_.store(!wasPlaying, std::memory_order_relaxed);
}

double Player::position() const {
    if (hasAudio_.load(std::memory_order_relaxed))
        return static_cast<double>(playedFrames_.load(std::memory_order_relaxed)) / kSampleRate;

    const std::lock_guard lock(mutex_);
    if (!playing_.load(std::memory_order_relaxed)) return silentPosition_;
    return silentPosition_ + std::chrono::duration<double>(std::chrono::steady_clock::now() - resumedAt_).count();
}

std::string Player::details() const {
    const std::lock_guard lock(mutex_);
    return details_;
}

void Player::fillAudio(std::int16_t* output, std::size_t frames) {
    const std::size_t wanted = frames * kChannels;
    std::size_t given = 0;

    if (playing_.load(std::memory_order_relaxed)) {
        const std::lock_guard lock(mutex_);
        while (given < wanted && ringFill_ > 0) {
            output[given++] = ring_[ringRead_];
            ringRead_ = (ringRead_ + 1) % ring_.size();
            --ringFill_;
        }
    }
    std::fill(output + given, output + wanted, std::int16_t{0});
    playedFrames_.fetch_add(given / kChannels, std::memory_order_relaxed);
}

void Player::pushAudio(const std::int16_t* samples, std::size_t count, const std::stop_token& stop) {
    std::size_t written = 0;
    while (written < count && !stop.stop_requested()) {
        {
            const std::lock_guard lock(mutex_);
            while (written < count && ringFill_ < ring_.size()) {
                ring_[ringWrite_] = samples[written++];
                ringWrite_ = (ringWrite_ + 1) % ring_.size();
                ++ringFill_;
            }
        }
        if (written < count) std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

void Player::pushFrame(Frame&& frame, const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        {
            const std::lock_guard lock(mutex_);
            if (frames_.size() < kMaxFrames) {
                frames_.push_back(std::move(frame));
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

std::optional<Frame> Player::takeFrame() {
    const double now = position();
    const std::lock_guard lock(mutex_);

    std::optional<Frame> due;
    while (!frames_.empty() && frames_.front().timestamp <= now + 0.004) {
        due = std::move(frames_.front());
        frames_.pop_front();
    }
    return due;
}

void Player::run(std::stop_token stop) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    Source source = openSource(path_);
    if (!source.reader || (source.video == kNoStream && source.audio == kNoStream)) {
        failed_.store(true, std::memory_order_relaxed);
        CoUninitialize();
        return;
    }

    hasVideo_.store(source.video != kNoStream, std::memory_order_relaxed);
    hasAudio_.store(source.audio != kNoStream, std::memory_order_relaxed);
    width_.store(source.width, std::memory_order_relaxed);
    height_.store(source.height, std::memory_order_relaxed);
    duration_.store(source.duration, std::memory_order_relaxed);
    {
        const std::lock_guard lock(mutex_);
        details_ = source.details;
    }

    ma_device device{};
    bool deviceReady = false;
    if (source.audio != kNoStream) {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_s16;
        config.playback.channels = kChannels;
        config.sampleRate = kSampleRate;
        config.dataCallback = audioCallback;
        config.pUserData = this;

        if (ma_device_init(nullptr, &config, &device) == MA_SUCCESS && ma_device_start(&device) == MA_SUCCESS)
            deviceReady = true;
        else
            hasAudio_.store(false, std::memory_order_relaxed);  // fall back to the wall clock
    }

    bool videoDone = source.video == kNoStream;
    bool audioDone = source.audio == kNoStream;

    while (!stop.stop_requested() && !(videoDone && audioDone)) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* rawSample = nullptr;
        if (FAILED(source.reader->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &streamIndex, &flags, &timestamp,
                                             &rawSample)))
            break;
        const ComPtr<IMFSample> sample{rawSample};

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (streamIndex == source.video) videoDone = true;
            if (streamIndex == source.audio) audioDone = true;
        }
        if (!sample) continue;

        const auto buffer =
            query<IMFMediaBuffer>([&](IMFMediaBuffer** out) { return sample->ConvertToContiguousBuffer(out); });
        if (!buffer) continue;

        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;

        if (streamIndex == source.video) {
            Frame frame;
            frame.width = source.width;
            frame.height = source.height;
            frame.timestamp = static_cast<double>(timestamp) / 1e7;
            frame.bgra.resize(static_cast<std::size_t>(source.width) * source.height * 4);

            const std::size_t row = static_cast<std::size_t>(source.width) * 4;
            const std::size_t pitch = static_cast<std::size_t>(std::abs(source.stride));
            for (int y = 0; y < source.height; ++y) {
                const int line = source.stride < 0 ? source.height - 1 - y : y;  // negative stride: bottom-up
                std::memcpy(frame.bgra.data() + y * row, data + line * pitch, row);
            }

            buffer->Unlock();
            pushFrame(std::move(frame), stop);
            continue;
        }

        if (streamIndex == source.audio)
            pushAudio(reinterpret_cast<const std::int16_t*>(data), length / sizeof(std::int16_t), stop);
        buffer->Unlock();
    }

    finished_.store(true, std::memory_order_relaxed);
    if (deviceReady) ma_device_uninit(&device);
    CoUninitialize();
}

}  // namespace pb::media
