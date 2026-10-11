# 下屏位图字体与字形表

下屏的文字全部来自**这两张随仓库提交的点阵字形表**，运行时不做任何字体光栅化：

| 生成脚本 | 产物 | 内容 |
|---|---|---|
| `tools/secondscreen/gen_cjk_atlas.py` | `app/jni/src/src/ss_cjk_data.h` | 简体中文字形，16×16 格子 |
| `tools/secondscreen/gen_latin_sheet.py` | `app/jni/src/src/ss_latin_data.h` | `0-9` 与 `A-Z`，8×16 格子 |

两张表都是 2bpp 编码（`0`＝透明、`1`＝描边、`2`＝本体），所以绘制代码只需按值写
颜色即可，不需要 alpha 混合。字体随仓库提交，这样**离线就能重新生成**，CI 也可以
跑 `--check` 守住"改了文案却忘了重跑脚本"。

## 为什么是 Unifont 16px

下屏是 320×240 物理像素，正文以 1.0 倍绘制，字形按原始像素 1:1 落到屏幕上，
因此字号只能取**点阵字体的原生档位**——非原生尺寸会渲染成空白。

| 候选 | 结论 |
|---|---|
| Fusion Pixel 10px / 12px | 墨迹 9×9 / 11×11，字身框有富余，偏小 |
| **GNU Unifont 16px** | **墨迹 15×16，填满字身框；中文 20 992 字形；且是纯 1-bit 点阵** ✅ |
| 方舟像素 16px | 只有 **97** 个 CJK 字形，是半成品 ❌ |
| 思源黑体 / Noto Sans SC（矢量） | 12px 抗锯齿后糊成一团，16px 才勉强可用，且必须把图集改成带 alpha ❌ |

Unifont 的另一个优势是**它自带半宽拉丁**（8×16），与 16×16 的 CJK 共享同一个 em 框，
所以中文和拉丁来自同一字体、落在同一基线上。此前下屏的 0-9 与 A-Z 取自游戏自带的
8×8 tile 字模（约 5×7 墨迹），和 16px 中文并排会明显脱节。

## 文件

| 文件 | 用途 |
|---|---|
| `unifont-18.0.01.otf` | **当前使用**。CJK 与拉丁都取自这里（16px 原生点阵档） |
| `unifont-LICENSE.txt` | Unifont 的许可全文（OFL 1.1 与 GPLv2+ 双许可） |
| `fusion-pixel-12px-monospaced-zh_hans.ms.bitmap.ttf` | 上一版（3.2.3）用的中文档，保留以便对比 |
| `fusion-pixel-10px-monospaced-zh_hans.ms.bitmap.ttf` | 更早的档位，同样保留 |
| `OFL.txt` | Fusion Pixel 的 SIL Open Font License 1.1 |

Unifont 是**双许可**：SIL OFL 1.1，以及 GPLv2+ 含 GNU 字体嵌入例外。本项目按
**OFL 1.1** 使用。

## 来源与校验和

- [GNU Unifont](https://unifoundry.com/unifont/) 18.0.01
  （`https://unifoundry.com/pub/unifont/unifont-18.0.01/font-builds/unifont-18.0.01.otf`）
- [Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font) release
  [`2026.08.11`](https://github.com/TakWolf/fusion-pixel-font/releases/tag/2026.08.11)，
  资产 `…-12px-monospaced-ms.bitmap.ttf-…zip`（asset id `509931730`）与
  `…-10px-monospaced-ms.bitmap.ttf-…zip`（asset id `509931660`）

```
88d0a14d4aa9a96419720b39ba5da921a59560d76d4ec7d523acc06ed85cd3c2  unifont-18.0.01.otf
1e74cb82bf476843e97c2596297b04219b1a7e51f7238944a8c031cb9401fa87  unifont-LICENSE.txt
4e443b6e40c19ecb9bb08bc85091b7d5f48357f3b994c60850a3e3a797fc55f2  fusion-pixel-12px-monospaced-zh_hans.ms.bitmap.ttf
17df8b7af3def5f81f57346623e239464e9d72c66d2f2f4bb2db1893168eee32  fusion-pixel-10px-monospaced-zh_hans.ms.bitmap.ttf
bc518cf64b8032c07690f33cc270c35c179255a6ac8efa7c165ebae7e8f76a63  OFL.txt
```

## 重新生成

依赖 **Python 3 + Pillow**：

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install Pillow

python tools/secondscreen/gen_cjk_atlas.py   --check
python tools/secondscreen/gen_latin_sheet.py --check

# 重新生成
python tools/secondscreen/gen_cjk_atlas.py
python tools/secondscreen/gen_latin_sheet.py
```

`gen_cjk_atlas.py` 会自动扫描 `second_screen.c`、`second_screen_sdl.c`、`updater.c`
里的中文字符，不需要手工维护字表；**改了下屏中文文案后必须重跑**，否则新字在
图集里没有对应字形，会显示成空白。

拉丁表的字符集固定在 `gen_latin_sheet.py` 的 `CHARS`（`0-9` 后接 `A-Z`），
索引顺序与 `ss_sheets.h` 里的 `kSS_TextDigitCell` / `kSS_TextLetterCell` 对应。
`draw_text()` 会把小写折成大写，所以只需要大写。

> 若要切回旧的 Fusion Pixel 档位，除了 `--font` 还要把脚本里的 `BOX` / `OFF_X` /
> `OFF_Y` 常量一并改回（见脚本注释），并同步调整 `second_screen_sdl.c` 的版面：
> 那些常量是按 16px 文字块调的。
