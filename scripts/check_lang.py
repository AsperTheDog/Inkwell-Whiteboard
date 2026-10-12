#!/usr/bin/env python3
"""Keeps the language files in app/assets/lang healthy.

  python scripts/check_lang.py                  check every language against English and against the source code
  python scripts/check_lang.py --new es Español start a language: a copy of en.txt to translate
  python scripts/check_lang.py --missing es     print the ids es.txt still lacks, as "id=English text" lines

Errors (exit code 1): an id used in the source that en.txt does not define, an id in a language that English does
not have, a placeholder ({0}, {1}...) that differs from the English text.
Warnings: texts a language has not translated yet (they show in English), English texts nothing in the source uses.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANG_DIR = ROOT / "app" / "assets" / "lang"
SOURCE_DIRS = [ROOT / "app" / "src", ROOT / "core" / "src"]

ID_LITERAL = re.compile(r'"([a-z][A-Za-z0-9]*(?:\.[A-Za-z0-9-]+)+)"')
PLACEHOLDER = re.compile(r"\{(\d+)\}")
# Ids built at run time ("shortcuts.<section>.<n>.keys"): never reported as unused
DYNAMIC_PREFIXES = ("shortcuts.",)


def parse(path: Path) -> dict:
    texts = {}
    raw = path.read_text(encoding="utf-8-sig")
    for number, line in enumerate(raw.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            print(f"{path.name}:{number}: not an id=text line: {line[:60]}")
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if key in texts:
            print(f"{path.name}:{number}: {key} is defined twice")
        texts[key] = value.strip()
    return texts


def placeholders(text: str) -> set:
    return set(PLACEHOLDER.findall(text.replace("{{", "").replace("}}", "")))


TR_CALL = re.compile(r"\btr\(")


def tr_arguments(line: str):
    """The text inside each tr( ... ) on a line, parentheses matched"""
    for match in TR_CALL.finditer(line):
        depth = 1
        end = match.end()
        while end < len(line) and depth > 0:
            depth += {"(": 1, ")": -1}.get(line[end], 0)
            end += 1
        yield line[match.end():end]


def source_ids(prefixes: set) -> tuple:
    """(every id-looking literal in the source, and those inside a tr() call, each with where it is)"""
    found = {}
    called = {}
    for directory in SOURCE_DIRS:
        for path in list(directory.rglob("*.cpp")) + list(directory.rglob("*.cppm")):
            for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if line.lstrip().startswith("//"):
                    continue
                where = f"{path.relative_to(ROOT)}:{number}"
                for match in ID_LITERAL.finditer(line):
                    if match.group(1).split(".")[0] in prefixes:
                        found.setdefault(match.group(1), where)
                # Widget keys ("settings.close") look like ids too: only what tr() is given counts as one
                for arguments in tr_arguments(line):
                    for match in ID_LITERAL.finditer(arguments):
                        called.setdefault(match.group(1), where)
    return found, called


def font_characters(path: Path) -> set:
    """The characters a font file has glyphs for, or None when fontTools is not installed"""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        return None
    font = TTFont(path, fontNumber=0, lazy=True)
    return {chr(code) for code in font.getBestCmap()}


def uncovered(path: Path, texts: dict):
    """Characters of a language's texts that neither Inter nor the language's font can draw"""
    inter = font_characters(ROOT / "app" / "assets" / "fonts" / "Inter.ttf")
    if inter is None:
        return None
    covered = set(inter)
    name = texts.get("language.font", "")
    if name and (ROOT / "app" / "assets" / "fonts" / "ui" / name).exists():
        covered |= font_characters(ROOT / "app" / "assets" / "fonts" / "ui" / name)
    used = set("".join(texts.values()).replace("\n", " "))
    return sorted(c for c in used - covered if c not in " 	")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--new", nargs=2, metavar=("CODE", "NAME"), help="create CODE.txt from en.txt")
    parser.add_argument("--missing", metavar="CODE", help="list what CODE.txt has not translated")
    args = parser.parse_args()

    english = parse(LANG_DIR / "en.txt")

    if args.new:
        code, name = args.new
        target = LANG_DIR / f"{code}.txt"
        if target.exists():
            print(f"{target} exists already")
            return 1
        text = (LANG_DIR / "en.txt").read_text(encoding="utf-8")
        text = re.sub(r"^language\.name=.*$", f"language.name={name}", text, count=1, flags=re.M)
        target.write_text(text, encoding="utf-8", newline="\n")
        print(f"Created {target}. Translate the texts after each '='; whatever is left in English still works.")
        return 0

    if args.missing:
        other = parse(LANG_DIR / f"{args.missing}.txt")
        for key, text in english.items():
            if key not in other:
                print(f"{key}={text}")
        return 0

    errors = 0
    prefixes = {key.split(".")[0] for key in english}

    used, called = source_ids(prefixes)
    # Only literals on a tr() line are certainly ids: others may be widget keys that look alike ("settings.close")
    for key, where in sorted(called.items()):
        if key not in english:
            print(f"error: {where}: {key} is not defined in en.txt")
            errors += 1
    unused = [key for key in english if key not in used and not key.startswith(DYNAMIC_PREFIXES) and key != "language.name"]
    for key in unused:
        print(f"warning: en.txt: {key} is not used by the source")

    for path in sorted(LANG_DIR.glob("*.txt")):
        if path.name == "en.txt":
            continue
        texts = parse(path)
        if "language.name" not in texts:
            print(f"warning: {path.name}: language.name is missing, the picker shows '{path.stem}'")
        for key, text in texts.items():
            if key not in english:
                print(f"error: {path.name}: {key} does not exist in en.txt")
                errors += 1
            elif placeholders(text) != placeholders(english[key]):
                print(f"error: {path.name}: {key} uses placeholders {sorted(placeholders(text))}, English has {sorted(placeholders(english[key]))}")
                errors += 1
        # An empty English text ("language.font") is a setting that most languages leave alone
        translatable = [key for key, text in english.items() if text]
        missing = [key for key in translatable if key not in texts]
        done = len(translatable) - len(missing)
        print(f"{path.stem}: {done}/{len(translatable)} texts ({100 * done // len(translatable)}%)")
        if missing:
            print(f"warning: {path.name}: {len(missing)} texts missing (run with --missing {path.stem})")
        lacking = uncovered(path, texts)
        if lacking:
            print(f"warning: {path.name}: no font has {len(lacking)} characters ({''.join(lacking[:30])}...). "
                  "Name a font with language.font and run scripts/make_ui_fonts.py")

    print("ok" if errors == 0 else f"{errors} error(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
