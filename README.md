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
| `Enter` | play / pause, while an audio or video preview is open |

The tray icon's context menu has a single entry, *Exit PeekaBoo*.

While PeekaBoo runs it swallows `Space` whenever Explorer or the desktop has focus, so
Explorer's type-ahead search cannot contain spaces during that time. That is the same trade-off
the original macOS shortcut makes.

## What gets previewed

| Type | Rendering |
| --- | --- |
| `png` `jpg` `bmp` `gif` `tga` `psd` `hdr` `ppm` `pgm` | decoded with stb_image, uploaded as a GPU texture |
| `webp` `tiff` `ico` `heic` `avif` `jxr` | decoded by Windows Imaging Component, which picks up whatever codecs are installed |
| `txt` `md` and ~40 code/config extensions | monospace text, virtualised so only visible lines cost anything |
| `mp4` `mkv` `mov` `avi` `webm` and friends | played with sound, picture and a timeline |
| `mp3` `wav` `flac` `m4a` `aac` `wma` | played, with the embedded cover art if there is one |
| anything else | whatever the shell itself shows for the file, plus name, size and the reason |

Files without a known extension are sniffed: if the first 4 KB look like text, they are rendered
as text. Images go to stb_image first and fall back to WIC, which is how formats stb cannot read
still work. The last fallback asks the shell through `IShellItemImageFactory`, so PDFs and Office
documents get the same thumbnail Explorer would show, and everything else gets its file type icon.

## Playing media

Audio and video play through Media Foundation - no ffmpeg, no vendored binaries, it is already
part of Windows. `IMFSourceReader` demuxes and decodes, and is told to hand back plain RGB32
frames and 48 kHz stereo PCM, so the format conversions and the scaling happen inside Windows'
own video processor rather than in our code. Video larger than the panel is scaled down there
too, which keeps both the frame queue and the upload cost small.

Playback keeps the same rule as everything else: **the render thread never decodes**. A worker
thread pulls samples, pushes PCM into a ring buffer and frames into a short queue, and blocks
once either fills - that back-pressure is what paces decoding to playback. The audio device
drains the ring and doubles as the clock; each frame the render thread asks which picture belongs
to the current playback position, drops anything late, and uploads at most one texture. Files
without an audio track fall back to a wall clock.

Frames go to the GPU as `GL_BGRA`, the byte order Media Foundation already produces, so no pixel
is touched on the way. Pausing silences the audio callback rather than stopping the device: the
device only notices a stop between samples, which let the clock drift past the pause.

## Performance design

The whole point of the exercise. Opening a preview costs **a couple of milliseconds** when the
file was prefetched, which is the normal case while browsing with the arrow keys.

- **Nothing decodes on the render thread.** A `std::jthread` worker pool (cores - 1) does all
  file I/O, decoding and shell icon extraction. Results are handed back through a queue and
  picked up by the render thread, which is the only thread that ever touches OpenGL.
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
- **Idle costs nothing.** With no preview open the loop parks in `glfwWaitEventsTimeout` and
  renders nothing at all. While open it is capped at 60 FPS - a static panel has no business
  driving a high refresh display.

The panel footer shows the live numbers: time from keypress to visible content, whether it was a
cache hit, frame rate and cache usage.

## How the panel looks the way it does

The panel sizes itself to its content, up to 1366 x 768: an image or video gets a window that
hugs it and is never scaled up, a fallback gets a small one, text gets the full size. It is centred on whichever
monitor Explorer is on.

Its rounded corners come from `DWMWA_WINDOW_CORNER_PREFERENCE`. The obvious alternative,
`SetWindowRgn`, opts the window out of DWM per-pixel alpha - which is why rounded corners and a
translucent background could never both work while that was in use. It also produced two visibly
different radii, because `CreateRoundRectRgn` takes ellipse diameters where the drawn border took
a radius.

The frosted backdrop is ours, not the compositor's. Windows composites this OpenGL window
opaquely: it reports `GLFW_TRANSPARENT_FRAMEBUFFER` as granted but ignores the alpha channel, and
neither `DwmEnableBlurBehindWindow`, nor `SetWindowCompositionAttribute` in any accent state, nor
`DWMWA_SYSTEMBACKDROP_TYPE` changes that. Real acrylic would mean rendering through
DirectComposition instead of a WGL swap chain, on top of undocumented DWM exports. So PeekaBoo
grabs the screen itself while the panel is still hidden and shrinks it to an eighth with
`StretchBlt` in `HALFTONE` mode - that averaging *is* the blur - then stretches it back underneath
the panel, where bilinear filtering smooths it further. The snapshot covers the whole monitor, so
arrowing on to another file only moves the sampled UV rectangle; nothing is grabbed again.

`kBlurDivisor` sets the blur radius and `kPanelTint` how much of it reads through.

## Architecture

Five translation units, flat, no framework:

| File | Responsibility |
| --- | --- |
| `src/main.cpp` | tray icon, low-level keyboard hook, event loop |
| `src/shell.cpp` | Windows shell integration: which file is selected in the focused Explorer/desktop view |
| `src/media.cpp` | audio and video playback: Media Foundation decoding, audio output, clock |
| `src/preview.cpp` | worker pool, memory mapped I/O, image and text decoding, shell icons |
| `src/overlay.cpp` | the preview panel: GPU texture cache, backdrop, layout, drawing |

The keyboard hook only sets a flag - it never does real work, because it runs inline with the
system's input processing and a slow hook gets silently unhooked.

`shell.cpp` binds to the focused Explorer window through `IShellWindows` -> `IShellBrowser` ->
`IFolderView2` and caches that binding, so polling the selection every frame is a single cheap
COM call rather than a full enumeration.

The preview window is borderless, topmost, `WS_EX_NOACTIVATE` and `WS_EX_TOOLWINDOW`: it never
takes focus away from Explorer - which is what makes the arrow keys keep working - and never
shows up in the taskbar or Alt+Tab.

## Build

Requires CMake 3.24+, a C++20 compiler and network access on the first configure - GLFW,
Dear ImGui, stb and miniaudio are fetched by `FetchContent` at pinned versions. Everything else
comes from Windows itself.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Tested with MinGW g++ 15.2 and Ninja on Windows 11. MSVC works as well; the target is built as a
Windows subsystem executable, with `/ENTRY:mainCRTStartup` so `int main()` stays the entry point.

## Known limitations

- Windows only. The decoding layer is portable (there is a POSIX `mmap` path), but the shell
  integration, the keyboard hook and the backdrop are Win32.
- The backdrop is frozen: it shows the screen as it was when the preview opened. Anything moving
  underneath while it is open is not picked up.
- Explorer's own sort order is not read; neighbour prefetch uses a name sort of the folder.
- Media files are never prefetched - browsing a folder of videos would otherwise start a decoder
  per neighbour. They show the shell's poster until the player has its first frame.
- Playback cannot be seeked, and Media Foundation brings no Ogg Vorbis or Opus.
- Mouse wheel scrolling in text previews relies on the Windows "scroll inactive windows on hover"
  setting, which is on by default, because the panel never takes focus.
