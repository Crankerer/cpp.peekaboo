<p align="center">
  <img src="doc/icon.png" alt="PeekaBoo" width="128">
</p>

<h1 align="center">PeekaBoo</h1>

<p align="center">Quick Look for Windows. Select a file, press <b>Space</b>, look at it.</p>

---

Pick a file in **Windows Explorer** or on the **desktop**, tap **Space**, and the preview panel is
simply there. Tap **Space** or **Esc** to make it go away.

Leave it open and walk through the folder with the **arrow keys**: Explorer moves its selection and
the preview follows along without a loading hitch.

PeekaBoo has no window of its own. It sits in the notification area and stays out of the way until
you ask for something.

![An image preview](doc/preview-image.png)

## Getting started

1. Run `peekaboo.exe`. A small eye icon appears in the notification area.
2. Click a file in Explorer and press **Space**.
3. Right-click the tray icon and switch on **Start with Windows** if you want it there for good.

## Controls

| Key | What it does |
| --- | --- |
| `Space` | open the preview for the selected file, or close it again |
| `Esc` | close the preview |
| arrow keys | move the Explorer selection; the open preview follows |
| `Enter` | play / pause an audio or video preview |

The tray icon's right-click menu has two entries: **Start with Windows**, a checkmark you can
toggle at any time, and **Exit PeekaBoo**. Autostart is off until you turn it on, and it only ever
writes a single entry for your own user account — nothing system-wide, no administrator rights.

One trade-off worth knowing: while PeekaBoo runs it takes `Space` whenever Explorer or the desktop
has focus, so Explorer's type-ahead search cannot contain spaces during that time. The macOS
shortcut this borrows from makes exactly the same bargain.

## Playing audio and video

![A video preview with the transport controls](doc/preview-video.png)

Media files get a full transport row under the picture:

| Control | What it does |
| --- | --- |
| ▶ / ❚❚ | play or pause — the `Enter` key does the same |
| ⏪ ⏩ | jump ten seconds back or forward |
| progress bar | click anywhere to jump there; hold and drag to scrub |
| volume slider | on the right edge — drag it, or roll the **mouse wheel** anywhere over the panel |

The volume you set carries over to the next file you preview. Audio-only files show their embedded
cover art in place of the picture.

## What can be previewed

Everything below works out of the box — there is nothing to install, no codec pack, no plugins.

| Files | How they are shown |
| --- | --- |
| `png` `jpg` `jpeg` `bmp` `gif` `tga` `psd` `hdr` `ppm` `pgm` | decoded directly and shown at full quality |
| `webp` `tiff` `tif` `ico` `heic` `heif` `avif` `jxr` | decoded by Windows itself, using whatever image codecs you have installed |
| `txt` `md` `csv` `log` `json` `xml` `yaml` `ini` and ~40 code extensions | as monospace text, scrollable, even for very large files |
| `mp4` `m4v` `mov` `mkv` `avi` `wmv` `webm` `mpg` `mpeg` `ts` `flv` | played with sound, picture and the transport controls |
| `mp3` `wav` `flac` `m4a` `aac` `wma` | played, with the embedded cover art if the file has one |
| `pdf`, Office documents, anything else | the same thumbnail Explorer shows, plus name, size and file type |

A file with no extension, or an unknown one, is sniffed: if the first few kilobytes look like text,
it is shown as text. Otherwise you get the shell's thumbnail or the file type icon, so pressing
Space never leaves you staring at nothing.

![A text preview](doc/preview-text.png)

### What does not work

| Not supported | Why, and what happens instead |
| --- | --- |
| `ogg` `opus` audio and video | Windows ships no decoder for them. The panel says the file cannot be decoded. |
| exotic codecs inside a supported container | An `mkv` or `avi` is only as playable as the codec inside it. Same message. |
| animated `gif`, `webp` | Shown as a still image — the first frame. |
| RAW camera files (`cr2`, `nef`, `arw`, …) | Only if you installed the vendor's or Microsoft's RAW codec; otherwise the shell thumbnail. |
| `svg` | No vector rendering. You get the shell thumbnail. |
| PDF and Office documents | Not rendered as documents, only the shell thumbnail — the same picture Explorer shows. |
| Subtitles, multiple audio tracks | The first audio track is played; subtitle tracks are ignored. |
| Text beyond 1 MiB | The rest is cut off; the footer says so. |

If a file cannot be decoded, the **Open with …** button at the top right hands it to the
application that owns the file type.

## How fast it is

The panel footer shows live numbers while it is open: how long the preview took from keypress to
picture, whether the file was already prepared, the frame rate and the cache usage.

Browsing a folder with the arrow keys is the case it is built for. PeekaBoo prepares the two
neighbours in each direction ahead of time, so by the time you step to the next file its preview is
already decoded and sitting on the graphics card. On the machine these screenshots were taken on
that reads as **around 30 ms** for a prepared file against roughly 180 ms for one decoded on the
spot. Decoded previews are kept in a 512 MB cache, and nothing is ever decoded on the thread that
draws, which is why the panel does not stutter even on a large file.

At rest PeekaBoo does nothing at all: no preview open means no frames drawn. With one open it caps
itself at 60 FPS, because a mostly static panel has no business driving a 240 Hz display.

## Building it yourself

You need CMake 3.24+, a C++20 compiler and — on the first configure only — a network connection, so
GLFW, Dear ImGui, stb and miniaudio can be fetched at pinned versions. Everything else comes from
Windows.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

The result is `build\peekaboo.exe`, a single executable with no install step and no runtime to
deploy. Tested with MinGW g++ 15.2 and with MSVC on Windows 11.

## Under the hood, briefly

Windows only. Media playback goes through Media Foundation, which is already part of the system —
no ffmpeg, no bundled binaries. The selection is read from Explorer through the shell's own
interfaces, and `Space` is caught with a low-level keyboard hook, which is what lets the panel
appear without ever stealing focus from Explorer.

The frosted backdrop is drawn by PeekaBoo rather than by Windows: the screen is captured the moment
before the panel appears and shrunk hard, which is what produces the blur. That also means it is a
still picture — anything moving behind the panel while it is open is not picked up.

Six source files, no framework:

| File | Responsibility |
| --- | --- |
| `src/main.cpp` | tray icon, keyboard and mouse hooks, event loop |
| `src/shell.cpp` | which file is selected in the focused Explorer or desktop view |
| `src/media.cpp` | audio and video: decoding, output, clock, seeking |
| `src/preview.cpp` | worker pool, file I/O, image and text decoding, shell icons |
| `src/overlay.cpp` | the panel itself: layout, drawing, transport and volume controls |
| `src/startup.cpp` | the autostart entry behind the tray menu toggle |

The icon is vector art; `doc/icon.svg` is the master that `src/peekaboo.ico` is generated from.

## Known limitations

- Windows only.
- The backdrop is frozen while the panel is open.
- Explorer's own sort order is not read; neighbour prefetch walks the folder by name.
- Media files are not prepared ahead of time — a folder of videos would otherwise start a decoder
  per neighbour. They show the shell's poster until the first frame arrives.
- Mouse wheel scrolling in text previews relies on the Windows "scroll inactive windows on hover"
  setting, which is on by default, because the panel never takes focus. Over a media preview the
  wheel always works, since PeekaBoo picks it up itself to set the volume.
