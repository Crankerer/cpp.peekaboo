#include "shell.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <memory>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace pb::shell {
namespace {

template <class T>
struct Release {
    void operator()(T* p) const noexcept { p->Release(); }
};
template <class T>
using ComPtr = std::unique_ptr<T, Release<T>>;

// Wraps the "pass an out-parameter, check the HRESULT" pattern COM is built on.
template <class T, class Call>
ComPtr<T> query(Call&& call) {
    T* raw = nullptr;
    return SUCCEEDED(call(&raw)) ? ComPtr<T>{raw} : ComPtr<T>{};
}

std::wstring classOf(HWND window) {
    wchar_t name[64]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return name;
}

bool isShellView(std::wstring_view cls) {
    return cls == L"CabinetWClass" || cls == L"ExploreWClass" || cls == L"Progman" || cls == L"WorkerW";
}

bool isDesktop(std::wstring_view cls) { return cls == L"Progman" || cls == L"WorkerW"; }

ComPtr<IFolderView2> viewOf(IDispatch* dispatch) {
    const auto provider =
        query<IServiceProvider>([&](IServiceProvider** out) { return dispatch->QueryInterface(IID_PPV_ARGS(out)); });
    if (!provider) return {};

    const auto browser = query<IShellBrowser>(
        [&](IShellBrowser** out) { return provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(out)); });
    if (!browser) return {};

    const auto shellView = query<IShellView>([&](IShellView** out) { return browser->QueryActiveShellView(out); });
    if (!shellView) return {};

    return query<IFolderView2>([&](IFolderView2** out) { return shellView->QueryInterface(IID_PPV_ARGS(out)); });
}

ComPtr<IFolderView2> bind(HWND target) {
    const auto windows = query<IShellWindows>(
        [](IShellWindows** out) { return CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(out)); });
    if (!windows) return {};

    if (isDesktop(classOf(target))) {
        VARIANT anywhere{};
        long desktop = 0;
        const auto dispatch = query<IDispatch>([&](IDispatch** out) {
            return windows->FindWindowSW(&anywhere, &anywhere, SWC_DESKTOP, &desktop, SWFO_NEEDDISPATCH, out);
        });
        return dispatch ? viewOf(dispatch.get()) : ComPtr<IFolderView2>{};
    }

    long count = 0;
    if (FAILED(windows->get_Count(&count))) return {};

    for (long i = 0; i < count; ++i) {
        VARIANT index{};
        index.vt = VT_I4;
        index.lVal = i;

        const auto dispatch = query<IDispatch>([&](IDispatch** out) { return windows->Item(index, out); });
        if (!dispatch) continue;

        const auto app =
            query<IWebBrowserApp>([&](IWebBrowserApp** out) { return dispatch->QueryInterface(IID_PPV_ARGS(out)); });
        if (!app) continue;

        SHANDLE_PTR handle = 0;
        if (FAILED(app->get_HWND(&handle)) || reinterpret_cast<HWND>(handle) != target) continue;
        return viewOf(dispatch.get());
    }
    return {};
}

HWND bound = nullptr;
ComPtr<IFolderView2> view;

}  // namespace

bool initialize() { return SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)); }

void shutdown() {
    forget();
    CoUninitialize();
}

bool explorerHasFocus() {
    HWND foreground = GetForegroundWindow();
    return foreground && isShellView(classOf(foreground));
}

void forget() {
    view.reset();
    bound = nullptr;
}

std::optional<fs::path> selectedFile() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || !isShellView(classOf(foreground))) return std::nullopt;

    if (foreground != bound || !view) {
        view = bind(foreground);
        bound = foreground;
    }
    if (!view) return std::nullopt;

    const auto items = query<IShellItemArray>([&](IShellItemArray** out) { return view->GetSelection(FALSE, out); });
    if (!items) return std::nullopt;

    DWORD count = 0;
    if (FAILED(items->GetCount(&count)) || count != 1) return std::nullopt;

    const auto item = query<IShellItem>([&](IShellItem** out) { return items->GetItemAt(0, out); });
    if (!item) return std::nullopt;

    PWSTR display = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &display))) return std::nullopt;
    fs::path path(display);
    CoTaskMemFree(display);

    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return std::nullopt;
    return path;
}

}  // namespace pb::shell
