# PeekaBoo

A macOS style Quick Look for Windows: select a file in **Windows Explorer** or on the **desktop**,
press **Space**, and a preview panel appears instantly. Press **Space** or **Esc** to close it.
Keep the preview open and use the **arrow keys** - Explorer moves its selection and the preview
follows without a loading hitch.

The app itself is headless. It lives in the notification area and has no window of its own until
you ask for a preview.

## Status

Proof of concept. It does what the acceptance criteria ask for, and the performance machinery
behind it (worker pool, prefetch, LRU GPU cache, memory mapped I/O) is real rather than sketched.

## Usage

```bash
build\peekaboo.exe
```

| Key | Action |
| --- | --- |
| `Space` | open the preview for the selected file, or close it again |
| `Esc` | close the preview |
| arrow keys | move the Explorer selection; the open preview follows |

The tray icon's context menu has a single entry, *Exit PeekaBoo*.

While PeekaBoo runs it swallows `Space` whenever Explorer or the desktop has focus, so
Explorer's type-ahead search cannot contain spaces during that time. That is the same trade-off
the original macOS shortcut makes.

## What gets previewed

| Type | Rendering |
| --- | --- |
| `png` `jpg` `bmp` `gif` `tga` `psd` `hdr` `ppm` `pgm` | decoded with stb_image, uploaded as a GPU texture |
| `txt` `md` and ~40 code/config extensions | monospace text, virtualised so only visible lines cost anything |
| anything else | fallback card with type badge, name, size and the reason |

Files without a known extension are sniffed: if the first 4 KB look like text, they are rendered
as text. `webp` is detected but not decodable - stb_image has no WebP support, so it shows the
fallback card with that reason. PDF rendering would need a third-party library and is not
included.

## Performance design

The whole point of the exercise. Opening a preview costs **well under a millisecond** when the
file was prefetched, which is the normal case while browsing with the arrow keys.

- **Nothing decodes on the render thread.** A `std::jthread` worker pool (cores - 1) does all
  file I/O and decoding. Results are handed back through a queue and picked up by the render
  thread, which is the only thread that ever touches OpenGL.
- **Prefetching.** On every selection change the two neighbours in each direction are queued at
  low priority; the selected file jumps the queue. Stale prefetch jobs are dropped once the
  queue grows past 24 entries, so hammering the arrow keys never builds a backlog.
- **LRU cache with a byte budget.** Decoded previews are cached with a 512 MB budget. Images
  count their GPU footprint; the CPU side pixel buffer is released right after upload. The file
  currently on screen is never evicted.
- **Memory mapped I/O.** Files are mapped, not read, so decoding works straight off the mapping
  with no intermediate copy. `MappedFile` is the single place that talks to the raw OS file API.
- **Bounded frame work.** At most two texture uploads per frame, and the visible preview always
  gets the first slot. Text previews cap at 1 MiB and draw through `ImGuiListClipper`, so a 4 MB
  log file renders as fast as a two-line note.
- **Downscaled decodes.** Images larger than 2048 px on the long edge are box-filtered down on
  the worker thread, which keeps uploads cheap and memory bounded. The panel still reports the
  original resolution.

The panel footer shows the live numbers: time from keypress to visible content, whether it was a
cache hit, frame rate and cache usage.

## Architecture

Four translation units, flat, no framework:

| File | Responsibility |
| --- | --- |
| `src/main.cpp` | tray icon, low-level keyboard hook, event loop |
| `src/shell.cpp` | Windows shell integration: which file is selected in the focused Explorer/desktop view |
| `src/preview.cpp` | worker pool, memory mapped I/O, image and text decoding |
| `src/overlay.cpp` | the preview panel: GPU texture cache, layout, drawing |

The event loop idles in `glfwWaitEventsTimeout` while no preview is open, so a hidden PeekaBoo
costs no measurable CPU. The keyboard hook only sets a flag - it never does real work, because it
runs inline with the system's input processing.

`shell.cpp` binds to the focused Explorer window through `IShellWindows` -> `IShellBrowser` ->
`IFolderView2` and caches that binding, so polling the selection every frame is a single cheap
COM call rather than a full enumeration.

The preview window is borderless, topmost, `WS_EX_NOACTIVATE` and `WS_EX_TOOLWINDOW`: it never
takes focus away from Explorer (which is what makes the arrow keys keep working) and never shows
up in the taskbar or Alt+Tab. Its rounded corners come from `SetWindowRgn`, so the look does not
depend on the compositor granting a transparent framebuffer.

## Build

Requires CMake 3.24+, a C++20 compiler and network access on the first configure - GLFW,
Dear ImGui and stb are fetched by `FetchContent` at pinned versions.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Tested with MinGW g++ 15.2 and Ninja on Windows 11. MSVC works as well; the target is built as a
Windows subsystem executable, with `/ENTRY:mainCRTStartup` so `int main()` stays the entry point.

## Known limitations

- Windows only. The decoding layer is portable (there is a POSIX `mmap` path), but the shell
  integration and the keyboard hook are Win32.
- Explorer's own sort order is not read; neighbour prefetch uses a name sort of the folder.
- Clicking inside the panel does not steal focus, but mouse wheel scrolling in text previews
  relies on the Windows "scroll inactive windows on hover" setting, which is on by default.
- No PDF, no video, no thumbnails from shell extensions.
