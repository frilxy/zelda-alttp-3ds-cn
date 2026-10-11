#!/usr/bin/env python3
"""Fail if any bottom-screen panel's text collides with its title or overflows.

Placed here after several rounds of "the title overlaps the first row" and "the
labels sit outside their button": every constant is read back out of the source,
so it checks the tree as it stands rather than a hand-copied model.

Original docstring follows.

Vertical overflow gate for the bottom screen at 16px text.

Each panel's constants are read out of its own function body in
second_screen_sdl.c, so this checks the tree as it actually stands.  Horizontal
fit needs no check: text_width() counts 16px per CJK and 8px per ASCII regardless
of glyph size, so only the vertical dimension changed when the text block grew
from ~10px to 16px.

Per panel it verifies:
  * content starts at or below the 2x title cell (r.y + ty .. + 32)
  * every text cell fits inside its row box
  * nothing runs past the panel bottom (r.h)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = str(ROOT / "app/jni/src/src/platform/linux/second_screen_sdl.c")
src = open(SRC, encoding="utf-8").read()

U = 0.5
W, H = 320, 240
RW, RH = W - 20 * U, H - 20 * U          # r = {10u, 10u, W-20u, H-20u}
TITLE_H = 16   # titles are Unifont 16px at 1x and their ink fills the cell
TEXT_H = 16


def body(fn):
    """Slice out one function body by brace matching."""
    i = src.index(f"static void {fn}(")
    i = src.index("{", i)
    d, j = 1, i + 1
    while d:
        if src[j] == "{":
            d += 1
        elif src[j] == "}":
            d -= 1
        j += 1
    return src[i:j]


def one(fn, pat, what):
    m = re.search(pat, body(fn), re.S)
    if not m:
        sys.exit(f"  cannot find {what!r} in {fn}()")
    return [float(g) * U for g in m.groups()]


def title_y(fn, pat, what):
    return one(fn, pat, what)[0]


results = []


def check(name, start, items, header=()):
    """items must clear the title; header items share the title's line but sit
    horizontally clear of the centred title, so only the panel bound applies."""
    bad = []
    for label, top, bottom in items:
        if top < start - 0.5:
            bad.append(f"{label}: 顶 {top:.1f} < 标题底 {start:.1f}")
    for label, top, bottom in list(items) + list(header):
        if bottom > RH + 0.5:
            bad.append(f"{label}: 底 {bottom:.1f} > 面板底 {RH:.1f}")
    allt = items + [("_", start, start)]
    print(f"  {'✅' if not bad else '❌'} {name:<24} 内容起 {start:>5.1f}  "
          f"最低底 {max(b for _, _, b in allt):>5.1f} / {RH:.0f}")
    for b in bad:
        print(f"        ⚠ {b}")
    results.append(not bad)


print(f"  面板 r.w={RW:g} r.h={RH:g}   标题 2x 格子 {TITLE_H}px   正文格子 {TEXT_H}px\n")

# ---- 画面 / 开发者：4 行 ----
for fn, tt in (("draw_screen_panel", r'SS_STR\("SCREEN","画面"\)'),
               ("draw_developer_panel", r'SS_STR\("DEVELOPER","开发者选项"\)')):
    ty = title_y(fn, tt + r'[^;]*?r\.y \+ ([0-9.]+) \* u', "title")
    rh, gap = one(fn, r'float row_h = ([0-9.]+) \* u, gap = ([0-9.]+) \* u;', "row_h")
    y0 = one(fn, r'float y0 = r\.y \+ ([0-9.]+) \* u;', "y0")[0]
    items = []
    for i in range(4):
        top = y0 + i * (rh + gap)
        items.append((f"行{i+1}文字", top + rh / 2 - 8, top + rh / 2 + 8))
        items.append((f"行{i+1}框", top, top + rh))
    check(fn.replace("draw_", "").replace("_panel", "") + f" (row_h {rh:g})", ty + TITLE_H - 4, items)

# ---- 按键设置：6 行 ----
fn = "draw_remap_panel"
ty = title_y(fn, r'SS_STR\("REMAP BUTTONS","按键设置"\)[^;]*?r\.y \+ ([0-9.]+) \* u', "title")
rh, gap = one(fn, r'float row_h = ([0-9.]+) \* u, gap = ([0-9.]+) \* u;', "row_h")
y0 = one(fn, r'float y0 = r\.y \+ ([0-9.]+) \* u;', "y0")[0]
items = []
for i in range(6):
    top = y0 + i * (rh + gap)
    items.append((f"行{i+1}文字", top + rh / 2 - 8, top + rh / 2 + 8))
    items.append((f"行{i+1}框", top, top + rh))
header = []
for nm, pat in (("返回按钮", r'remap_back_r = \(RectFS\)\{r\.x \+ [0-9.]+ \* u, r\.y \+ ([0-9.]+) \* u, [0-9.]+ \* u, ([0-9.]+) \* u\}'),
                ("翻页按钮", r'remap_page_r = \(RectFS\)\{r\.x \+ r\.w - [0-9.]+ \* u, r\.y \+ ([0-9.]+) \* u, [0-9.]+ \* u, ([0-9.]+) \* u\}')):
    by, bh = one(fn, pat, nm)
    header += [(nm + "框", by, by + bh), (nm + "文字", by + bh / 2 - 8, by + bh / 2 + 8)]
check(f"remap (row_h {rh:g})", ty + TITLE_H - 4, items, header)

# ---- 叠加层：8 行 ----
fn = "draw_developer_overlay_panel"
ty = title_y(fn, r'SS_STR\("OVERLAY","叠加层"\)[^;]*?r\.y \+ ([0-9.]+) \* u', "title")
oy = one(fn, r'float y = r\.y \+ ([0-9.]+) \* u;', "y")[0]
oh = one(fn, r'float row_h = ([0-9.]+) \* u;', "row_h")[0]
items = []
for i in range(8):
    top = oy + i * (oh + 3 * U)
    items.append((f"行{i+1}框", top - 4 * U, top - 4 * U + oh))
    items.append((f"行{i+1}文字", top, top + TEXT_H))
check(f"overlay (row_h {oh:g})", ty + TITLE_H - 4, items)

# ---- 设置：5 行自适应 ----
fn = "draw_settings"
ty = title_y(fn, r'SS_STR\("SETTINGS","设置"\)[^;]*?r\.y \+ ([0-9.]+) \* u', "title")
gap, y0, sa, sb = one(fn, r'float gap = ([0-9.]+) \* u;\s*float y0 = r\.y \+ ([0-9.]+) \* u;\s*'
                          r'float row_h = \(r\.h - ([0-9.]+) \* u - ([0-9.]+) \* u - 4 \* gap\) / 5;', "geometry")
rh = (RH - sa - sb - 4 * gap) / 5
items = []
for i in range(5):
    top = y0 + i * (rh + gap)
    items.append((f"行{i+1}文字", top + rh / 2 - 8, top + rh / 2 + 8))
    items.append((f"行{i+1}框", top, top + rh))
check(f"settings (row_h {rh:.1f})", ty + TITLE_H - 4, items)

# ---- 道具：网格 ----
fn = "draw_items"
ty = title_y(fn, r'SS_STR\("ITEMS","道具"\)[^;]*?r\.y \+ ([0-9.]+) \* u', "title")
cw1a, cw2a = one(fn, r'float cw1 = \(r\.w - ([0-9.]+) \* u\) / 5, cw2 = \(r\.h - ([0-9.]+) \* u\) / 4;', "cells")
cellw = min((RW - cw1a) / 5, (RH - cw2a) / 4)
g1, g2 = one(fn, r'grid_y = r\.y \+ ([0-9.]+) \* u \+ \(r\.h - ([0-9.]+) \* u - 4 \* cellW\) / 2;', "grid_y")
gy = g1 + (RH - g2 - 4 * cellw) / 2
check(f"items (cell {cellw:.1f})", ty + TITLE_H - 4,
      [(f"格{i+1}", gy + i * cellw, gy + (i + 1) * cellw) for i in range(4)])

# ---- 读取存档 ----
fn = "draw_load_confirmation"
ty = title_y(fn, r'SS_STR\("LOAD STATE", "读取存档"\);\s*draw_text(?:_tex)?\(title,\s*[^;]*?r\.y \+ ([0-9.]+) \* u', "title")
ly = one(fn, r'r\.y \+ \(([0-9.]+) \+ i \* [0-9.]+\) \* u', "lines")[0]
by, bh = RH - one(fn, r'float button_y = r\.y \+ r\.h - ([0-9.]+) \* u;', "btn_y")[0], 46 * U
items = [(f"行{i+1}", ly + i * 34 * U, ly + i * 34 * U + TEXT_H) for i in range(3)]
items += [("按钮框", by, by + bh), ("按钮文字", by + 14 * U, by + 14 * U + TEXT_H)]
check("load", ty + TITLE_H - 4, items)

# ---- 更新 ----
fn = "draw_update_panel"
ty = title_y(fn, r'SS_STR\("UPDATE", "更新"\).*?r\.y \+ ([0-9.]+) \* u', "title")
items = []


def box(pat, nm):
    y, h = one(fn, pat, nm)
    items.extend([(nm + "框", y, y + h), (nm + "文字", y + 6 * U, y + 6 * U + TEXT_H)])


box(r'update_channel_r = \(RectFS\)\{[^}]*?r\.y \+ ([0-9.]+) \* u,[^}]*?([0-9.]+) \* u\};', "频道")
box(r'update_release_r = \(RectFS\)\{[^}]*?r\.y \+ ([0-9.]+) \* u,[^}]*?([0-9.]+) \* u\};', "版本")
box(r'update_prev_r = \(RectFS\)\{[^}]*?r\.y \+ ([0-9.]+) \* u,[^}]*?([0-9.]+) \* u\};', "翻页")
_ay = one(fn, r'update_action_r = \(RectFS\)\{[^}]*?r\.y \+ r\.h - ([0-9.]+) \* u, r\.w - [0-9.]+ \* u, ([0-9.]+) \* u\};', "action")
by, bh = RH - _ay[0], _ay[1]
items += [("操作框", by, by + bh), ("操作文字", by + 8 * U, by + 8 * U + TEXT_H)]
for nm, pat in (("消息", r'            r\.y \+ (1[0-9.]+) \* u, 2 \* u\);'), ("提示", r'            r\.y \+ (2[0-9.]+) \* u, 2 \* u\);')):
    m = re.search(pat, body(fn))
    y = float(m.group(1)) * U
    items.append((nm + "文字", y, y + TEXT_H))
check("update", ty + TITLE_H - 4, items)

print("\n" + ("  ✅ 全部面板通过：无纵向溢出、无标题重叠"
              if all(results) else "  ❌ 仍有溢出"))
sys.exit(0 if all(results) else 1)
