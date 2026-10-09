#!/usr/bin/env python3
"""Packs a finished Release build into an archive that runs after extraction.

    scripts/package_release.py windows   -> dist/Inkwell-<version>-windows-x64.zip
    scripts/package_release.py linux     -> (run inside WSL / on Linux) dist/Inkwell-<version>-linux-x64.tar.gz

Build first: `cmake --build build --config Release` on Windows, `scripts/build_linux.sh release` on Linux.
Linux needs glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39 and later) and Vulkan 1.2+ drivers.
Windows also bundles the Visual C++ runtime DLLs, so no redistributable has to be installed.
"""
import glob
import os
import shutil
import sys
import tarfile
import tempfile
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DIST = os.path.join(ROOT, "dist")
VERSION = "1.0.0"

README = """Inkwell {version}
==================

An infinite-canvas whiteboard with pen, highlighter, shapes, ruler, text, pictures, videos and PDF import.

Run {exe} to start. Nothing has to be installed besides a graphics driver with Vulkan 1.2 or newer.

  Ctrl+S / Ctrl+O   save / open a board (.wbrd)
  Ctrl+I            insert a picture, video or PDF
  Ctrl+Shift+E      export the board as PNG
  F3                diagnostics

Boards and settings are stored in your user profile, never next to the program, so this folder can be moved or deleted
freely. Third-party licenses are in the licenses folder.
"""


def fail(p_message):
    print("error: " + p_message, file=sys.stderr)
    sys.exit(1)


def copy_tree(p_source, p_dest, p_skip=()):
    for l_dir, l_dirs, l_files in os.walk(p_source):
        l_dirs[:] = [d for d in l_dirs if d not in p_skip]
        l_out = os.path.join(p_dest, os.path.relpath(l_dir, p_source))
        os.makedirs(l_out, exist_ok=True)
        for l_file in l_files:
            if l_file not in p_skip:
                shutil.copy2(os.path.join(l_dir, l_file), os.path.join(l_out, l_file))


def stage_common(p_build, p_stage, p_exe, p_platform):
    if not os.path.isfile(os.path.join(p_build, p_exe)):
        fail("%s not found in %s: build the Release configuration first" % (p_exe, p_build))
    shutil.copy2(os.path.join(p_build, p_exe), os.path.join(p_stage, p_exe))
    copy_tree(os.path.join(p_build, "shaders"), os.path.join(p_stage, "shaders"))
    # assets/test and the assets README are for developers
    copy_tree(os.path.join(p_build, "assets"), os.path.join(p_stage, "assets"), p_skip=("test", "README.md"))

    l_licenses = os.path.join(p_stage, "licenses")
    os.makedirs(l_licenses)
    shutil.copy2(os.path.join(ROOT, "vendor", "ffmpeg", p_platform, "LICENSE.txt"), os.path.join(l_licenses, "FFmpeg-LICENSE.txt"))
    shutil.copy2(os.path.join(ROOT, "vendor", "pdfium", p_platform, "LICENSE"), os.path.join(l_licenses, "PDFium-LICENSE.txt"))
    copy_tree(os.path.join(ROOT, "vendor", "pdfium", p_platform, "licenses"), os.path.join(l_licenses, "PDFium-third-party"))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(l_licenses, "Inkwell-LICENSE.txt"))
    shutil.copy2(os.path.join(ROOT, "THIRD-PARTY-NOTICES.txt"), os.path.join(l_licenses, "THIRD-PARTY-NOTICES.txt"))
    with open(os.path.join(p_stage, "README.txt"), "w", newline="\n") as l_file:
        l_file.write(README.format(version=VERSION, exe=p_exe))


def windows():
    l_name = "Inkwell-%s-windows-x64" % VERSION
    l_build = os.path.join(ROOT, "build", "app", "Release")
    l_stage = os.path.join(DIST, l_name)
    shutil.rmtree(l_stage, ignore_errors=True)
    os.makedirs(l_stage)
    stage_common(l_build, l_stage, "inkwell.exe", "win64")

    # SDL, PDFium, and only the FFmpeg libraries the program imports (not avdevice / avfilter)
    for l_dll in ["SDL3.dll", "pdfium.dll", "avformat-62.dll", "avcodec-62.dll", "avutil-60.dll", "swscale-9.dll", "swresample-6.dll"]:
        shutil.copy2(os.path.join(l_build, l_dll), l_stage)

    # Visual C++ runtime, app-local
    l_redist = sorted(glob.glob(r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT"))
    if not l_redist:
        fail("Visual C++ redistributable DLLs not found")
    for l_dll in ["vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"]:
        shutil.copy2(os.path.join(l_redist[-1], l_dll), l_stage)

    l_archive = os.path.join(DIST, l_name + ".zip")
    if os.path.exists(l_archive):
        os.remove(l_archive)
    with zipfile.ZipFile(l_archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as l_zip:
        for l_dir, _, l_files in os.walk(l_stage):
            for l_file in sorted(l_files):
                l_path = os.path.join(l_dir, l_file)
                l_zip.write(l_path, os.path.join(l_name, os.path.relpath(l_path, l_stage)))
    return l_archive


def linux():
    l_name = "Inkwell-%s-linux-x64" % VERSION
    l_build = os.path.join(ROOT, "build-linux-release", "app")
    # Staged outside the repo: the libraries are symlinks, which a Windows-mounted folder cannot hold
    l_stage = os.path.join(tempfile.mkdtemp(prefix="wb-package-"), l_name)
    os.makedirs(l_stage)
    stage_common(l_build, l_stage, "inkwell", "linux64")

    # Shared libraries next to the executable (it has RPATH $ORIGIN). The unversioned .so names are link-time only.
    l_needed = ["libSDL3", "libpdfium", "libavformat", "libavcodec", "libavutil", "libswscale", "libswresample"]
    l_sdl = os.path.join(ROOT, "build-linux-release", "_deps", "sdl3_src-build")
    for l_lib in l_needed:
        l_found = [f for d in (l_build, l_sdl) for f in glob.glob(os.path.join(d, l_lib + ".so*")) if os.path.basename(f) != l_lib + ".so" or l_lib == "libpdfium"]
        if not l_found:
            fail("%s not found in %s" % (l_lib, l_build))
        # The build copies each library under every name; keep the longest-named file and link the others to it
        l_real = max(l_found, key=len)
        for l_file in l_found:
            l_dest = os.path.join(l_stage, os.path.basename(l_file))
            if l_file == l_real or os.path.getsize(l_file) != os.path.getsize(l_real) or os.path.islink(l_file):
                shutil.copy2(l_file, l_dest, follow_symlinks=False)
            else:
                os.symlink(os.path.basename(l_real), l_dest)

    l_archive = os.path.join(DIST, l_name + ".tar.gz")
    if os.path.exists(l_archive):
        os.remove(l_archive)
    with tarfile.open(l_archive, "w:gz", compresslevel=9) as l_tar:
        l_tar.add(l_stage, arcname=l_name)
    shutil.rmtree(os.path.dirname(l_stage), ignore_errors=True)
    return l_archive


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("windows", "linux"):
        fail("usage: package_release.py windows|linux")
    os.makedirs(DIST, exist_ok=True)
    l_result = windows() if sys.argv[1] == "windows" else linux()
    print("%s (%.1f MB)" % (l_result, os.path.getsize(l_result) / 1e6))
