# 下屏中文位图字体

`tools/secondscreen/gen_cjk_atlas.py` 用这里的字体把下屏中文渲染成
`app/jni/src/src/ss_cjk_data.h` 里的 16×16 字形图集。字体随仓库提交，这样：

- 离线就能重新生成图集，不需要先下载字体；
- CI 可以跑 `gen_cjk_atlas.py --check` 守住"改了文案却忘了重跑脚本"。

## 文件

| 文件 | 说明 |
|---|---|
| `fusion-pixel-10px-monospaced-zh_hans.ms.bitmap.ttf` | 10px 等宽简体中文点阵字面（原始位图字形，非缩放） |
| `OFL.txt` | 该字体及其衍生来源的 SIL Open Font License 1.1 |

## 来源

- 上游项目：[TakWolf/fusion-pixel-font](https://github.com/TakWolf/fusion-pixel-font)（SIL OFL 1.1）
- Release：tag [`2026.08.11`](https://github.com/TakWolf/fusion-pixel-font/releases/tag/2026.08.11)
- 资产：`fusion-pixel-font-10px-monospaced-ms.bitmap.ttf-v2026.08.11.zip`
  （asset id `509931660`，5 865 652 字节）
- 包内取出的两个文件：`fusion-pixel-10px-monospaced-zh_hans.ms.bitmap.ttf`、`OFL.txt`

校验和（`sha256sum`）：

```
17df8b7af3def5f81f57346623e239464e9d72c66d2f2f4bb2db1893168eee32  fusion-pixel-10px-monospaced-zh_hans.ms.bitmap.ttf
bc518cf64b8032c07690f33cc270c35c179255a6ac8efa7c165ebae7e8f76a63  OFL.txt
```

> 这把字体是**逐字节可复现**的：用它在 2026-10-11 重新生成 `ss_cjk_data.h`，
> 输出与仓库中已提交的文件完全一致，因此可以确认就是制作时使用的那一把。
> 换字体版本会改变字形，`--check` 会立刻报出来。

## 重新生成图集

依赖 **Python 3 + Pillow**（`gen_cjk_atlas.py` 只用到 `Image` / `ImageDraw` / `ImageFont`）：

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install Pillow

python tools/secondscreen/gen_cjk_atlas.py --check   # 只检查是否最新
python tools/secondscreen/gen_cjk_atlas.py           # 重新生成 ss_cjk_data.h
```

字体路径由脚本的 `DEFAULT_FONT` 指定；换字体用 `--font PATH` 覆盖。

**改了下屏中文文案后必须重跑**，否则新字在图集里没有对应字形，会显示成空白。
脚本会自动扫描 `second_screen.c`、`second_screen_sdl.c`、`updater.c` 里的中文字符，
不需要手工维护字表。
