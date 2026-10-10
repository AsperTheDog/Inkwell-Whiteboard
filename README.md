# Inkwell

> **Disclaimer:** this project has been heavily AI assisted. I mainly just wanted something free that would replace MS Whiteboard now that it has been shut down. AI has written most of the code under close supervision and guidance, I have tweaked both the AI and the code directly several times to keep things working and clean, but AI has done most of the work. I do not consider this to be a personal project of mine nor something from my portfolio. I published it in case someone found it useful since the app is genuinely good and covers most of what I thought anyone would need from such a tool. Contributions are welcome.

A native infinite-canvas whiteboard in the spirit of Microsoft Whiteboard. C++23 (named modules), Vulkan 1.3, SDL3.
Windows and Linux.

## Features

- Pen with pressure and tilt (Windows Ink, Wacom, Huion, XP-Pen), highlighter, shapes, ruler, adjustable stroke smoothing
- Text, pictures, animated GIFs, videos (with sound, of any size: a video is played straight from its file, never loaded into memory), PDF import
- Select, move, scale, rotate, align, group lock, duplicate, z-order
- Laser pointer with a fading trail
- Undo/redo, autosave with recovery, save file export/import, PNG export
- Light and dark themes
- Live collaboration over the network: shared editing, remote pointers, people list, live pen strokes, shared video playback

## Collaborating

One person hosts, the others join. Open **Collaborate** in the top bar.

- **Host a session...** picks a port (default 5000), a password and your display name. Others reach you at `your-address:port`.
- **Join a session...** takes the host's `address:port`, the password and your name.
- Everybody edits the same board: strokes, shapes, text, pictures, GIFs, videos and PDF pages. You see the others' pointers (in their colour, with their name), the people list at the top, and the lines they are drawing as they draw them. Undo only undoes your own edits.
- Pictures and videos stream after the board arrives; until a file is there you see a thumbnail with a spinner. A big video is written to a temporary file as it arrives and starts playing as soon as the beginning is there (a video whose index sits at the end of the file has to arrive first). Saving skips files that have not finished arriving and saves the rest.
- Video play/pause and the playhead are shared; loop, mute and volume are your own.

**Reaching the host.** The host needs an address the others can reach: the same local network, a VPN such as Tailscale or ZeroTier (easiest, nothing to open), or a router port forward to the chosen port. The host's firewall must allow the app.

**Security, plainly.** Traffic is plain TCP and is **not encrypted**. The password is never sent as text (it is used in a challenge-response), but anyone who can watch the connection can read the board. Use a local network or a VPN, and treat the password as a door lock, not a vault. The host checks everything clients send (sizes, ids, locks, rates) and treats them as untrusted, but anybody with the password can edit and delete anything on the board that is not locked. Anything you load or receive passes through the same validation as opening a file.

**Limits worth knowing.** Up to 64 people; files of any size that fit on the disk (a big one arrives in a temporary file on the receiving computer, which needs the room); interrupted file transfers restart from the beginning; the host's machine is the hub, so the session ends for everybody when the host stops.

## Running a release

Extract the archive and run `inkwell` / `inkwell.exe`. Needs a Vulkan 1.2 or newer driver (1.2 drivers must expose dynamic rendering, synchronization2 and demote-to-helper as extensions). The Linux build needs
glibc 2.38+ (Ubuntu 24.04, Debian 13, Fedora 39+).

## Building

Dependencies:

| Need | Notes |
|---|---|
| CMake 3.28+ | |
| C++23 compiler with modules | MSVC 19.34+ (Visual Studio) on Windows, Clang 17+ and Ninja on Linux |
| `slangc` | Vulkan SDK, or a Slang release (shaders) |
| Vulkan 1.2+ driver | validation layers optional (used in Debug) |
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
