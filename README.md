# Whiteboard

An infinite-canvas vector whiteboard in the spirit of Microsoft Whiteboard, written in C++23 (named modules) on
Vulkan 1.3 and SDL3.

## Building

The build is described in `whiteboard.pyke` and turned into CMake by [Pyke](https://github.com/AsperTheDog/Pyke).
The generated `CMakeLists.txt` files are committed, so plain CMake works without Pyke.

Requirements:

- A GPU and driver with **Vulkan 1.3** (any type: discrete, integrated or software such as llvmpipe).
- CMake 3.28+ and a compiler with C++20 modules support (MSVC 19.34+, Clang 17+).
- `slangc` (from the Vulkan SDK) to compile shaders.
- Optional: the Vulkan SDK validation layers (used automatically in debug builds).

### Windows

```powershell
git submodule update --init
pyke build            # Release in build/  (pyke build --debug for Debug)
# or: cmake -S . -B build && cmake --build build --config Debug
build\app\Debug\whiteboard.exe
```

### Linux / WSL

```bash
git submodule update --init
scripts/build_linux.sh debug --test     # clang + Ninja into build-linux-debug/
scripts/build_linux.sh debug --run
```

The script picks up the newest `~/toolchains/LLVM-*` and `~/toolchains/cmake-*` (or `CC`/`CXX` and PATH).
SDL3 needs the usual X11/Wayland development packages. Validation layers: `sudo apt install vulkan-validationlayers`.

### Useful switches

| Variable / flag | Effect |
|---|---|
| `WB_VALIDATION=0/1` | Force Vulkan validation off/on (default: on in Debug builds) |
| `WB_GPU=<substring>` | Pick the GPU whose name contains the substring |
| `--smoke-test[=N]` | Render N frames (default 120) and exit; exit code 3 on any validation message |

## Controls

| Key | Action |
|---|---|
| F3 | Toggle the debug overlay (GPU, frame times, pen diagnostics) |
| F11 | Toggle fullscreen |

**Tablets:** SDL3 reads pens through Windows Ink on Windows. For Wacom, Huion and XP-Pen tablets make sure
"Windows Ink" is enabled in the tablet driver settings; the F3 overlay shows live pressure and tilt.

## Layout

```
whiteboard.pyke    build description (source of truth for the CMake files)
core/              wb_core: platform-independent model (math, document, brush geometry, commands, I/O)
app/               the application: platform (SDL3), gfx (Vulkan), render, ui, tools, debug overlay
  shaders/         Slang shaders, compiled to SPIR-V next to the executable
  assets/          runtime assets copied next to the executable
tests/             GoogleTest unit tests for wb_core
libs/              generated wrapper targets for third-party libraries
vendor/            git submodules (imgui, stb)
cmake/             CMake helpers (shader compilation)
```

## Code conventions

- Allman braces, tabs, `m_` members, `l_` locals, `p_` parameters, `s_` statics, `PascalCase` types, `camelCase` functions.
- Each module is a `.cppm` interface plus an optional `.cpp` implementation unit (`module x;`).
- Third-party and standard headers are `#include`d in the global module fragment (`module;`) of every unit that
  uses them; they are never re-exported. In particular, code doing glm arithmetic includes `<glm/glm.hpp>` itself.
- Macros cannot cross module boundaries, so the few that are needed live in small headers (`gfx/vk_check.hpp`).
