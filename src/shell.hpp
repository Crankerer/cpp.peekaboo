#pragma once

#include <filesystem>
#include <optional>

// Talks to the Windows shell: which file is selected in the Explorer window or
// on the desktop that currently has focus. The active view is cached between
// calls so that polling it every frame stays cheap.
namespace pb::shell {

// Must be called once on the thread that runs the message loop.
bool initialize();
void shutdown();

// Cheap enough for the keyboard hook: only inspects the foreground window class.
[[nodiscard]] bool explorerHasFocus();

[[nodiscard]] std::optional<std::filesystem::path> selectedFile();

// Drops the cached view so we stop holding a reference into explorer.exe.
void forget();

}  // namespace pb::shell
