#!/usr/bin/env python3
"""Fail if a source file uses a symbol from a generated header it does not include.

This is the check that would have caught two real breakages:

  * second_screen.c used SS_TEXT_COLS (from platform/linux/ss_sheets.h) without
    including it -- it compiled in CI's bottom-screen variant only by accident.
  * The SDL file referenced tex_cjk_title / tex_cjk_tab while those were declared
    inside #ifdef BOTTOM_SCREEN_CN, so the standard target failed to compile.

Each header is scanned for the symbols it defines, then every .c file that
mentions one of those symbols must include that header (directly or through the
file it is generated into).  Pure Python, no build needed.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADERS = ["app/jni/src/src/ss_cjk_data.h",
           "app/jni/src/src/ss_cjk_large_data.h",
           "app/jni/src/src/platform/linux/ss_sheets.h",
           "app/jni/src/src/second_screen_tables.h"]
SOURCES = ["app/jni/src/src/second_screen.c",
           "app/jni/src/src/platform/linux/second_screen_sdl.c",
           "platform/3ds/source/update_view.h"]

SYM = re.compile(r"\b(?:#define|enum\s*\{|static\s+const\s+\w+\s+|\}\s*;)?\s*"
                 r"(kSSCjk\w*|kSS_\w+|kGlyph\w*|kIcon\w*|SS_TEXT_COLS|SS_LETTER_COLS|SS_GLYPH_COLS|kSSLatin\w*)\b")


def strip_comments(text):
    text = re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def main():
    provided = {}
    for h in HEADERS:
        p = ROOT / h
        if not p.exists():
            continue
        for sym in set(SYM.findall(strip_comments(p.read_text(encoding="utf-8")))):
            provided.setdefault(sym, []).append(Path(h).name)

    bad = 0
    for s in SOURCES:
        p = ROOT / s
        if not p.exists():
            continue
        raw = p.read_text(encoding="utf-8")
        code = strip_comments(raw)
        includes = {Path(i).name for i in re.findall(r'#include\s+[<"]([^">]+)[">]', raw)}
        for sym, headers in sorted(provided.items()):
            if not re.search(r"\b" + re.escape(sym) + r"\b", code):
                continue
            if any(Path(h).name in includes for h in headers):
                continue
            print(f"  FAIL {s}: uses {sym} but does not include {headers[0]}")
            bad += 1
    if bad:
        print(f"  {bad} unresolved symbol reference(s)")
        return 1
    print(f"  ok: every generated-header symbol is properly included ({len(provided)} symbols known)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
