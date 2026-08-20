#include "pdf.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <inspectable.h>
#include <objidl.h>
#include <roapi.h>
#include <shcore.h>
#include <shlwapi.h>
#include <winstring.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <memory>
#include <utility>

namespace fs = std::filesystem;

namespace pb::pdf {

// --- Windows.Data.Pdf, declared by hand --------------------------------------
// The MinGW SDK ships the WinRT core (roapi, winstring) but no projection for
// this namespace. IIDs and, more importantly, method order come straight from
// the Windows SDK's windows.data.pdf.h: the order *is* the ABI, so nothing here
// may be reordered or omitted, even the methods we never call.
//
// IAsyncInfo is declared here too rather than taken from <asyncinfo.h>, because
// that header puts it in the global namespace under MinGW and in
// ABI::Windows::Foundation under MSVC. One declaration builds with both.
//
// These must keep external linkage. Inside an anonymous namespace GCC can see
// the complete set of derived classes - none, because the implementations live
// in a Windows DLL - concludes that no call through them is reachable, and at
// -O3 turns the first one into a jump to nowhere.
namespace abi {

struct IPdfPage;
struct IPdfPageRenderOptions;

struct WinRtSize {
    float width;
    float height;
};
struct WinRtRect {
    float x, y, width, height;
};
struct WinRtColor {
    BYTE a, r, g, b;
};

struct IPdfDocument : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32 index, IPdfPage** page) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(unsigned char* value) = 0;
};

// IAsyncOperation<PdfDocument*>: the three slots after IInspectable are all we
// ever touch, and GetResults is the only one that differs from IAsyncAction.
struct IDocumentOperation;

struct IPdfDocumentStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(IInspectable* file, IDocumentOperation** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(IInspectable* file, HSTRING password,
                                                                   IDocumentOperation** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IUnknown* stream, IDocumentOperation** operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamWithPasswordAsync(IUnknown* stream, HSTRING password,
                                                                      IDocumentOperation** operation) = 0;
};

struct IPdfPage : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IUnknown* stream, IInspectable** action) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IUnknown* stream, IPdfPageRenderOptions* options,
                                                                     IInspectable** action) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(IInspectable** action) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(WinRtSize* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Dimensions(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Rotation(int* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PreferredZoom(float* value) = 0;
};

struct IPdfPageRenderOptions : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceRect(WinRtRect* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SourceRect(WinRtRect value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(WinRtColor* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BackgroundColor(WinRtColor value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsIgnoringHighContrast(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsIgnoringHighContrast(unsigned char value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BitmapEncoderId(GUID* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BitmapEncoderId(GUID value) = 0;
};

struct IDocumentOperation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IPdfDocument** result) = 0;
};

enum class AsyncState : int { Started = 0, Completed = 1, Canceled = 2, Error = 3 };

struct IAsyncState : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(AsyncState* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

}  // namespace abi

namespace {

using namespace abi;

constexpr int kMaxPageWidth = 1600;  // no point rasterising wider than the panel can show
constexpr auto kAwaitTimeout = std::chrono::seconds(20);

constexpr GUID kIidPdfDocumentStatics{
    0x433a0b5f, 0xc007, 0x4788, {0x90, 0xf2, 0x08, 0x14, 0x3d, 0x92, 0x25, 0x99}};
constexpr GUID kIidPdfPageRenderOptions{
    0x3c98056f, 0xb7cf, 0x4c29, {0x9a, 0x04, 0x52, 0xd9, 0x02, 0x67, 0xf4, 0x25}};
constexpr GUID kIidRandomAccessStream{
    0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
constexpr GUID kIidAsyncInfo{0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

template <class T>
struct Release {
    void operator()(T* p) const noexcept { p->Release(); }
};
template <class T>
using ComPtr = std::unique_ptr<T, Release<T>>;

struct HString {
    HSTRING value = nullptr;
    explicit HString(const wchar_t* text) {
        WindowsCreateString(text, static_cast<UINT32>(std::wcslen(text)), &value);
    }
    ~HString() {
        if (value) WindowsDeleteString(value);
    }
    HString(const HString&) = delete;
    HString& operator=(const HString&) = delete;
};

// Polls instead of installing a completion handler: this runs on our own worker
// thread, which has nothing else to do until the page comes out.
bool await(IInspectable* operation) {
    IAsyncState* rawInfo = nullptr;
    if (FAILED(operation->QueryInterface(kIidAsyncInfo, reinterpret_cast<void**>(&rawInfo)))) return false;
    const ComPtr<IAsyncState> info{rawInfo};

    const auto deadline = std::chrono::steady_clock::now() + kAwaitTimeout;
    AsyncState status = AsyncState::Started;
    while (std::chrono::steady_clock::now() < deadline) {
        if (FAILED(info->get_Status(&status)) || status != AsyncState::Started) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return status == AsyncState::Completed;
}

// Windows.Data.Pdf speaks IRandomAccessStream; the shell gives us plain IStream,
// and shcore bridges the two.
ComPtr<IUnknown> wrapStream(IStream* stream) {
    IUnknown* raw = nullptr;
    if (FAILED(CreateRandomAccessStreamOverStream(stream, BSOS_DEFAULT, kIidRandomAccessStream,
                                                  reinterpret_cast<void**>(&raw))))
        return {};
    return ComPtr<IUnknown>{raw};
}

std::vector<std::uint8_t> drain(IStream* stream) {
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return {};

    const auto size = static_cast<std::size_t>(stat.cbSize.QuadPart);
    if (size == 0 || size > (256u << 20)) return {};

    const LARGE_INTEGER start{};
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) return {};

    std::vector<std::uint8_t> bytes(size);
    ULONG read = 0;
    if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(size), &read))) return {};
    bytes.resize(read);
    return bytes;
}

}  // namespace

// --- lifetime ---------------------------------------------------------------
Document::Document(fs::path file) : path_(std::move(file)) {
    worker_ = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
}

Document::~Document() = default;

std::vector<Page> Document::pages() const {
    const std::lock_guard lock(mutex_);
    return pages_;
}

void Document::request(int index, int pixelWidth) {
    const std::lock_guard lock(mutex_);
    if (index < 0 || !claimed_.insert(index).second) return;
    queue_.emplace_back(index, std::clamp(pixelWidth, 64, kMaxPageWidth));
}

std::vector<Rendered> Document::collect() {
    const std::lock_guard lock(mutex_);
    return std::exchange(done_, {});
}

// --- the worker -------------------------------------------------------------
void Document::run(std::stop_token stop) {
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) {
        failed_.store(true, std::memory_order_relaxed);
        return;
    }

    {
        IStream* rawFile = nullptr;
        if (FAILED(SHCreateStreamOnFileEx(path_.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr,
                                          &rawFile))) {
            failed_.store(true, std::memory_order_relaxed);
            RoUninitialize();
            return;
        }
        const ComPtr<IStream> file{rawFile};
        const ComPtr<IUnknown> source = wrapStream(file.get());

        IPdfDocumentStatics* rawStatics = nullptr;
        const HString className(L"Windows.Data.Pdf.PdfDocument");
        if (!source || !className.value ||
            FAILED(RoGetActivationFactory(className.value, kIidPdfDocumentStatics,
                                          reinterpret_cast<void**>(&rawStatics)))) {
            failed_.store(true, std::memory_order_relaxed);
            RoUninitialize();
            return;
        }
        const ComPtr<IPdfDocumentStatics> statics{rawStatics};

        IDocumentOperation* rawOperation = nullptr;
        if (FAILED(statics->LoadFromStreamAsync(source.get(), &rawOperation)) || !rawOperation) {
            failed_.store(true, std::memory_order_relaxed);
            RoUninitialize();
            return;
        }
        const ComPtr<IDocumentOperation> operation{rawOperation};
        if (!await(operation.get())) {  // encrypted, damaged, or not a PDF at all
            failed_.store(true, std::memory_order_relaxed);
            RoUninitialize();
            return;
        }

        IPdfDocument* rawDocument = nullptr;
        if (FAILED(operation->GetResults(&rawDocument)) || !rawDocument) {
            failed_.store(true, std::memory_order_relaxed);
            RoUninitialize();
            return;
        }
        const ComPtr<IPdfDocument> document{rawDocument};

        UINT32 count = 0;
        document->get_PageCount(&count);
        pageCount_.store(static_cast<int>(count), std::memory_order_relaxed);

        // Page sizes up front, so the whole document can be laid out and scrolled
        // before a single page has been rasterised.
        std::vector<Page> sizes;
        sizes.reserve(count);
        for (UINT32 index = 0; index < count && !stop.stop_requested(); ++index) {
            IPdfPage* rawPage = nullptr;
            if (FAILED(document->GetPage(index, &rawPage)) || !rawPage) {
                sizes.push_back({612.0f, 792.0f});  // US Letter, a sane stand-in
                continue;
            }
            const ComPtr<IPdfPage> page{rawPage};

            WinRtSize size{612.0f, 792.0f};
            page->get_Size(&size);
            sizes.push_back({size.width, size.height});
        }
        {
            const std::lock_guard lock(mutex_);
            pages_ = std::move(sizes);
        }
        ready_.store(true, std::memory_order_relaxed);

        // --- serve render requests
        while (!stop.stop_requested()) {
            std::pair<int, int> job{-1, 0};
            {
                const std::lock_guard lock(mutex_);
                if (!queue_.empty()) {
                    job = queue_.front();
                    queue_.pop_front();
                }
            }
            if (job.first < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(6));
                continue;
            }

            IPdfPage* rawPage = nullptr;
            if (FAILED(document->GetPage(static_cast<UINT32>(job.first), &rawPage)) || !rawPage) continue;
            const ComPtr<IPdfPage> page{rawPage};

            IInspectable* rawOptions = nullptr;
            const HString optionsClass(L"Windows.Data.Pdf.PdfPageRenderOptions");
            if (FAILED(RoActivateInstance(optionsClass.value, &rawOptions)) || !rawOptions) continue;
            const ComPtr<IInspectable> optionsHolder{rawOptions};

            IPdfPageRenderOptions* rawRenderOptions = nullptr;
            if (FAILED(rawOptions->QueryInterface(kIidPdfPageRenderOptions,
                                                  reinterpret_cast<void**>(&rawRenderOptions))))
                continue;
            const ComPtr<IPdfPageRenderOptions> options{rawRenderOptions};
            options->put_DestinationWidth(static_cast<UINT32>(job.second));

            IStream* rawTarget = SHCreateMemStream(nullptr, 0);
            if (!rawTarget) continue;
            const ComPtr<IStream> target{rawTarget};
            const ComPtr<IUnknown> sink = wrapStream(target.get());
            if (!sink) continue;

            IInspectable* rawAction = nullptr;
            if (FAILED(page->RenderWithOptionsToStreamAsync(sink.get(), options.get(), &rawAction)) || !rawAction)
                continue;
            const ComPtr<IInspectable> action{rawAction};
            if (!await(action.get())) continue;

            const std::vector<std::uint8_t> encoded = drain(target.get());
            if (encoded.empty()) continue;

            ImageData image = decodeEncodedImage(encoded.data(), encoded.size());
            if (image.rgba.empty()) continue;

            const std::lock_guard lock(mutex_);
            done_.push_back(Rendered{job.first, std::move(image)});
        }
    }

    RoUninitialize();
}

}  // namespace pb::pdf
