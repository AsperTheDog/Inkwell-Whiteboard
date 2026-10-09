# Inkwell

> **Disclaimer:** this project has been heavily AI assisted. I mainly just wanted something free that would replace MS Whiteboard now that it has been shut down. AI has written most of the code under close supervision and guidance, I have tweaked both the AI and the code directly several times to keep things working and clean, but AI has done most of the work. I do not consider this to be a personal project of mine nor something from my portfolio. I published it in case someone found it useful since the app is genuinely good and covers most of what I thought anyone would need from such a tool. Contributions are welcome.

A native infinite-canvas whiteboard in the spirit of Microsoft Whiteboard. C++23 (named modules), Vulkan 1.3, SDL3.
Windows and Linux.

## Features

- Pen with pressure and tilt (Windows Ink, Wacom, Huion, XP-Pen), highlighter, shapes, ruler, adjustable stroke smoothing
- Text, pictures, animated GIFs, videos (with sound), PDF import
- Select, move, scale, rotate, align, group lock, duplicate, z-order
- Laser pointer with a fading trail
- Undo/redo, autosave with recovery, save file export/import, PNG export
- Light and dark themes

## Running a release

Extract the archive and run `inkwell` / `inkwell.exe`. Needs a Vulkan 1.3 driver. The Linux build needs
glibc 2.38+ (Ubuntu 24.04, Debian 13, Fedora 39+).

## Building

Dependencies:

| Need | Notes |
|---|---|
| CMake 3.28+ | |
| C++23 compiler with modules | MSVC 19.34+ (Visual Studio) on Windows, Clang 17+ and Ninja on Linux |
| `slangc` | Vulkan SDK, or a Slang release (shaders) |
| Vulkan 1.3 driver | validation layers optional (used in Debug) |
| Linux only | SDL3's usual X11/Wayland development packages |
| Python 3 / bash / curl | only for the fetch and packaging scripts |

SDL3, volk, Vulkan-Headers, VMA, glm, spdlog and GoogleTest are downloaded by CMake. Dear ImGui and stb are git
submodules. FFmpeg and PDFium are prebuilt binaries fetched by script (not in git).

```bash
git clone --recurse-submodules <repo>
cd inkwell
scripts/fetch_ffmpeg.sh            # win64 or linux64, picked automatically (LGPL shared build)
scripts/fetch_pdfium.sh
```

Windows:

```powershell
cmake -S . -B build
cmake --build build --config Release --target inkwell
build\app\Release\inkwell.exe
```

Linux (picks up `~/toolchains/LLVM-*` and `~/toolchains/cmake-*`, or `CC`/`CXX` and PATH):

```bash
scripts/build_linux.sh release [--test] [--run]    # into build-linux-release/
```

Tests: `cmake --build build --config Release --target wb_tests`, then `ctest` in `build/`.
Release archives: `python scripts/package_release.py windows|linux` (output in `dist/`).

The build is described in `inkwell.pyke` ([Pyke](https://github.com/AsperTheDog/Pyke)); the generated CMake files
are committed, so Pyke is not required.

Switches: `WB_VALIDATION=0/1`, `WB_GPU=<name substring>`, `--smoke-test[=N] [--smoke-ui=<mode>]`.

## Layout

```
core/      platform-independent model: math, document, brush geometry, commands, file I/O
app/       SDL3 platform, Vulkan renderer, UI, tools, shaders (Slang), assets
tests/     GoogleTest unit tests
scripts/   Linux build, FFmpeg/PDFium fetch, release packaging
```

## Licenses

Inkwell is MIT licensed ([LICENSE](LICENSE)). Third-party components and their licenses are listed in [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) and shipped
in the `licenses/` folder of each release. FFmpeg is used as the unmodified LGPL build and loaded dynamically.

<img width="1270" height="669" alt="inkwell" src="https://github.com/user-attachments/assets/5a2361f6-0e4a-498a-952c-3cf1cb878571" />
