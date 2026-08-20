#include "startup.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace pb::startup {
namespace {

constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kValue = L"PeekaBoo";

// The command Windows should run, quoted so a space in the path survives.
std::wstring command() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (written == 0) return {};
        if (written < path.size()) {  // truncated means the buffer was too small
            path.resize(written);
            return L'"' + path + L'"';
        }
        path.resize(path.size() * 2);
    }
}

}  // namespace

bool enabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kKey, kValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool setEnabled(bool on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;

    LSTATUS status = ERROR_SUCCESS;
    if (on) {
        const std::wstring line = command();
        status = line.empty()
                     ? ERROR_INVALID_DATA
                     : RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(line.c_str()),
                                      static_cast<DWORD>((line.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kValue);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;  // already gone is the state we wanted
    }

    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

}  // namespace pb::startup
