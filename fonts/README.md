# 下屏中文位图字体

`tools/secondscreen/gen_cjk_atlas.py` 用这里的字体把下屏中文渲染成
`app/jni/src/src/ss_cjk_data.h` 里的 16×16 字形图集。字体随仓库提交，这样：

- 离线就能重新生成图集，不需要先下载字体；
- CI 可以跑 `gen_cjk_atlas.py --check` 守住"改了文案却忘了重跑脚本"。

## 文件

| 文件 | 用途 |
|---|---|
| `fusion-pixel-12px-monospaced-zh_hans.ms.bitmap.ttf` | **正文**（下屏正文 + 上屏对话同款） |
| `unifont-18.0.01.otf` | **面板标题**，16px + 算法加粗 |
| `fusion-pixel-8px-monospaced-zh_hans.ms.bitmap.ttf` | 旧方案遗留（标题曾用 8px×2），保留以便回退对比 |
| `fusion-pixel-10px-monospaced-zh_hans.ms.bitmap.ttf` | 旧的 10px 字面，保留以便 `--font` 回退对比 |
| `OFL.txt` | 该字体及其衍生来源的 SIL Open Font License 1.1 |

两把都是**纯位图字面**（没有可缩放轮廓）：只有在自己那一档尺寸下才能光栅化，
请求其他尺寸会得到**空白字形**。这也是为什么放大只能靠运行时整数倍缩放，
而不是换字号。

## 为什么是 12px

下屏是 320×240 物理像素，正文在 `u = 0.5` 下以 1.0 倍绘制，所以字形按原始像素
1:1 落到屏幕上——12px 字面的墨迹是 11×11，正好填满原先 10px（墨迹 9×9）留下的
上下空隙，而**字身框仍是 16×16、进位仍是 16px**，因此版面无需改动。

再往上只有 16px，而 16px 的中文档在主流像素字体里**没有完整实现**
（方舟像素 16px 只有 97 个 CJK 字形）。矢量字体在 12px 会因抗锯齿糊成一团，
到 16px 才勉强可用，且需要把图集从 1-bit 掩码改成带 alpha 才存得下灰阶。

## 来源

- 上游项目：[TakWolf/fusion-pixel-font](https://github.com/TakWolf/fusion-pixel-font)（SIL OFL 1.1）
- Release：tag [`2026.08.11`](https://github.com/TakWolf/fusion-pixel-font/releases/tag/2026.08.11)
- 12px 资产：`fusion-pixel-font-12px-monospaced-ms.bitmap.ttf-v2026.08.11.zip`
  （asset id `509931730`，8 827 782 字节）
- 10px 资产：`fusion-pixel-font-10px-monospaced-ms.bitmap.ttf-v2026.08.11.zip`
  （asset id `509931660`，5 865 652 字节）
- 两包内各取出 `…-zh_hans.ms.bitmap.ttf`；`OFL.txt` 取自 12px 包

校验和（`sha256sum`）：

```
4e443b6e40c19ecb9bb08bc85091b7d5f48357f3b994c60850a3e3a797fc55f2  fusion-pixel-12px-monospaced-zh_hans.ms.bitmap.ttf
17df8b7af3def5f81f57346623e239464e9d72c66d2f2f4bb2db1893168eee32  fusion-pixel-10px-monospaced-zh_hans.ms.bitmap.ttf
bc518cf64b8032c07690f33cc270c35c179255a6ac8efa7c165ebae7e8f76a63  OFL.txt
```

## 重新生成图集

依赖 **Python 3 + Pillow**（`gen_cjk_atlas.py` 只用到 `Image` / `ImageDraw` / `ImageFont`）：

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install Pillow

python tools/secondscreen/gen_cjk_atlas.py --check   # 只检查是否最新
python tools/secondscreen/gen_cjk_atlas.py           # 重新生成 ss_cjk_data.h
python tools/secondscreen/gen_cjk_atlas.py --font fonts/fusion-pixel-10px-monospaced-zh_hans.ms.bitmap.ttf
```

字体路径由脚本的 `DEFAULT_FONT` 指定；用 `--font` 可切到 10px 那一档做对比
（切回去还要把 `BOX`/`OFF_X`/`OFF_Y` 常量一起改回来，见脚本注释）。

**改了下屏中文文案后必须重跑**，否则新字在图集里没有对应字形，会显示成空白。
脚本会自动扫描 `second_screen.c`、`second_screen_sdl.c`、`updater.c` 里的中文字符，
不需要手工维护字表。

## 两档字号

`gen_cjk_atlas.py` 一次生成**两张图集**，共用同一字符表，都按 16×16 整格贴图，
所以一个绘制偏移对两档都成立、版面常量无需区分：

| 图集 | 字面 | 绘制倍率 | 用途 |
|---|---|---|---|
| `ss_cjk_data.h` | Fusion Pixel 12px | 1× | 正文 |
| `ss_cjk_large_data.h` | GNU Unifont 16px + **算法加粗** | 1× | 面板标题、标签栏 |

### 标题的加粗是怎么来的

Unifont 只有 Regular 一个字重，没有 Bold，也没有可变轴。标题的粗体是**合成**的，
用的是像素字体界的「右移重叠」算法：

```text
solid  = 右移1px(bitmap) | bitmap
shadow = 右移1px(solid & ~bitmap)
result = solid & ~shadow
```

第三步是关键：把「因为右移而封住的镂空」减回去，所以笔画变粗但**字形内部空隙
不会被填死**（简单膨胀会把「图」「备」糊成实心）。参考实现见
[pixel-font-studio/fusion-bold-pixel-font](https://github.com/pixel-font-studio/fusion-bold-pixel-font)。

Unifont 16px 的汉字墨迹占 `x=0..14`，右侧正好空 1px，所以这个算法加出来的
那 1px 恰好落在格子内，**不溢出、不改格子尺寸、不动渲染偏移**。高度不变，
墨迹中心从 7 变为 7.5，而图集按整格贴图，这点偏差不影响版面。
