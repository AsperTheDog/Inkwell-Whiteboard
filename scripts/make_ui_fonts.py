#!/usr/bin/env python3
"""Builds the interface fonts in app/assets/fonts/ui: for each language file that names a font
("language.font=NotoSansJP-UI.ttf"), a copy of that language's Noto Sans reduced to the characters the language's
texts use (plus ASCII), so the interface is readable on machines without CJK fonts and stays a few hundred kB.

Run it again when a translation gains characters (scripts/check_lang.py reports uncovered ones):

  python scripts/make_ui_fonts.py              rebuild every font
  python scripts/make_ui_fonts.py --refresh    download the source fonts again

Needs `pip install fonttools`. The source fonts (OFL licence) are cached in scripts/.font_cache, which git ignores.
"""
import argparse
import sys
import urllib.request
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = Path(__file__).resolve().parent.parent
LANG_DIR = ROOT / "app" / "assets" / "lang"
OUT_DIR = ROOT / "app" / "assets" / "fonts" / "ui"
CACHE = Path(__file__).resolve().parent / ".font_cache"

BASE = "https://github.com/google/fonts/raw/main/ofl/"
# output file -> (source font, licence, layout features kept). CJK needs none of the shaping tables; scripts that join
# or reorder letters (Arabic, Hebrew...) need all of them.
SOURCES = {
    "NotoSansSC-UI.ttf": (BASE + "notosanssc/NotoSansSC%5Bwght%5D.ttf", BASE + "notosanssc/OFL.txt", ["kern", "vert"]),
    "NotoSansJP-UI.ttf": (BASE + "notosansjp/NotoSansJP%5Bwght%5D.ttf", BASE + "notosansjp/OFL.txt", ["kern", "vert"]),
    "NotoSansKR-UI.ttf": (BASE + "notosanskr/NotoSansKR%5Bwght%5D.ttf", BASE + "notosanskr/OFL.txt", ["kern", "vert"]),
    "NotoSansArabic-UI.ttf": (BASE + "notosansarabic/NotoSansArabic%5Bwdth%2Cwght%5D.ttf", BASE + "notosansarabic/OFL.txt", ["*"]),
    "NotoSansHebrew-UI.ttf": (BASE + "notosanshebrew/NotoSansHebrew%5Bwdth%2Cwght%5D.ttf", BASE + "notosanshebrew/OFL.txt", ["*"]),
}


def language_files() -> dict:
    """font file -> the text of every language file that uses it"""
    wanted = {}
    for path in sorted(LANG_DIR.glob("*.txt")):
        text = path.read_text(encoding="utf-8-sig")
        for line in text.splitlines():
            if line.startswith("language.font=") and line.split("=", 1)[1].strip():
                wanted.setdefault(line.split("=", 1)[1].strip(), []).append(text)
    return wanted


def download(url: str, refresh: bool) -> Path:
    CACHE.mkdir(exist_ok=True)
    target = CACHE / url.rsplit("/", 1)[1].replace("%5B", "[").replace("%5D", "]").replace("%2C", ",")
    if refresh or not target.exists():
        print("downloading", url)
        urllib.request.urlretrieve(url, target)
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--refresh", action="store_true", help="download the source fonts again")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, texts in language_files().items():
        if name not in SOURCES:
            print(f"error: {name} is named by a language file but has no source in this script")
            return 1
        url, licence_url, features = SOURCES[name]
        characters = {chr(c) for c in range(0x20, 0x7F)} | set("".join(texts)) | set("…—–·•")
        characters.discard("\n")

        font = TTFont(download(url, args.refresh))
        if "fvar" in font:
            # The regular weight at normal width: every axis at its default, except weight
            pins = {axis.axisTag: (400 if axis.axisTag == "wght" else axis.defaultValue) for axis in font["fvar"].axes}
            font = instancer.instantiateVariableFont(font, pins)

        options = subset.Options()
        options.hinting = False
        options.layout_features = features
        options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]  # names and the licence stay
        options.notdef_outline = True
        options.glyph_names = False
        options.drop_tables += ["DSIG", "STAT"]
        subsetter = subset.Subsetter(options)
        subsetter.populate(text="".join(sorted(characters)))
        subsetter.subset(font)
        target = OUT_DIR / name
        font.save(target)
        print(f"{name}: {len(characters)} characters, {target.stat().st_size // 1024} kB")

    licence = OUT_DIR / "LICENSE-noto.txt"
    if not licence.exists():
        urllib.request.urlretrieve(SOURCES["NotoSansSC-UI.ttf"][1], licence)  # the same licence for every Noto Sans
    return 0


if __name__ == "__main__":
    sys.exit(main())
