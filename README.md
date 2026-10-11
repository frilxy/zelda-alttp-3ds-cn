# Zelda: A Link to the Past 3DS — 简体中文版

Nintendo 3DS 双屏移植版《塞尔达传说：众神的三角力量》，本 fork **内置简体中文**。

基于以下开源工作：
- 逆向引擎 [snesrev/zelda3](https://github.com/snesrev/zelda3)
- 双屏 Android 基座 [Waterdish/zelda3-android](https://github.com/Waterdish/zelda3-android) / [samyost1/zelda3-android](https://github.com/samyost1/zelda3-android)
- 3DS 移植版 [EstebanPdN/zelda-alttp-3ds](https://github.com/EstebanPdN/zelda-alttp-3ds)
- 中文数据与字体：[LinkFish1/zelda3](https://github.com/LinkFish1/zelda3)、[Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font)（SIL OFL 开源）

本仓库**不包含 ROM**。你需要自备一份合法美版 ROM。

> ✅ 当前状态：**对话文本全中文**；**下屏（第二屏）菜单/UI 也已汉化**（在“下屏汉化版”中）。

---

## 两个版本

每次构建会同时出**两个版本**（下方下载里都有）：

| 版本 | 内容 |
|---|---|
| **标准版** `zelda3-3ds-v3.2.8` | 对话全中文，下屏仍为英文 |
| **下屏汉化版** `zelda3-3ds-v3.2.8-cn-bottom` | 对话 + **下屏菜单/设置/地图 UI 全中文** |

> 想要下屏中文就用 **`-cn-bottom`** 那个。

---

## 中文是怎么实现的

与上游“首次启动从 ROM 提取资源”不同，本 fork 采用 **LinkFish1 的“构建时烘焙”思路**：

- 把**已含中文的资源包 `zelda3_assets.dat`**（397 条中文对白 + 16×16 中文字体 + 字宽表 + `Language = cn` 标志）在构建时打包进 romfs；
- 移植了引擎字库渲染（`messaging.c` 的中文解码 + `VWF_RenderChinese`）；
- 下屏（第二屏）另外用 **10px Fusion Pixel 位图字体**渲染中文菜单（清晰、无描边）；
  字形表 `app/jni/src/src/ss_cjk_data.h` 由 `tools/secondscreen/gen_cjk_atlas.py`
  扫描下屏源码里的中文字符自动生成（当前 164 字），改文案后重跑该脚本即可；
- 更新器的状态提示（正在检查更新 / 下载校验失败 …）在下屏汉化版里也一并中文化；
- 启动时直接复制内置资源，并强制 `Language = cn` → **默认中文**，无需配置。

> 因此游戏逻辑用的是内置资源包；SD 上的美版 ROM 仅用于**建立游戏 profile**，不会再从它提取/汉化。

---

## 本 fork 的更新

### v3.2.8 — 标题/标签改用 Unifont 16px 原生点阵，并修正垂直居中

- **面板标题与底部标签改用 GNU Unifont 的 16px 原生点阵、1 倍绘制**
  （墨迹 15×16，**零缩放**，笔画绝对整齐）。此前是 12px 正文图集放 1.5 倍
  （16.5px），大小接近但非整数倍会导致笔画粗细不均。
- **修正"文字偏上"**：上一版标签的 y 算式写成了 `r.h/2 - 8*s`，在 1.5 倍下
  把格子顶端推到按钮边缘之外 1px。现在按实测文本框居中：标签 `r.h/2 - 8*u`
  （格子 9..25，中心 17 = 按钮中心），标题 `r.y + 20*u`（墨迹 6..22）。
- 代价：标题/标签用 Unifont，与正文的 Fusion Pixel 不同字体；这是为了拿到
  真正的 16px 且零缩放所必须的取舍。

### v3.2.7 — 标题与底部标签统一放大到 1.5 倍

- **底部标签（装备/地图/道具）改回 12px 字面、与正文同大**。v3.2.5 用的 10px
  字面在实机上偏小，而这三个标签紧挨正文行，同字号才协调。
- 顺带移除因此不再使用的 10px 图集（少一张 11 KB 图集与一个纹理）。
- **面板标题的 14px（8px 字面 ×2）不变**，实机确认大小合适。

### v3.2.5 — 回到 12px，标题分级改小

- **正文回到 Fusion Pixel 12px**（与上屏对话同字体、同字号），撤回 v3.2.4 的
  Unifont 16px——那是为了"更大"而牺牲了与上屏的一致性，得不偿失。
- **标题改用 8px 字面放大 2 倍（14px 墨迹）**。此前面板标题是 12px 的 2 倍即
  **22px**，明显过大；而 Fusion Pixel 只有 8/10/12px 三档原生尺寸，非整数倍又
  会发糊，所以 8px×2 是唯一能得到"比正文大、但不夸张"的整数方案。
- **底部标签改用 10px 字面（9px 墨迹）**，此前后是 `3 * u` = 1.5× 的 16.5px，
  既偏大又因非整数倍而发糊，现在两件事一起解决。
- **修掉三处遗漏的非整数倍缩放**：地图上的迷宫名（1.5×）、开发者叠加层的行
  （1.5×）、装备页标题（5×7 小字模的 1.4×）。它们都是**变量形式**的缩放参数，
  所以上一轮的整数倍扫描没覆盖到。
- 三张图集共用同一墨迹中心 (7,7)，因此绘制偏移与全部版面常量无需改动。

### v3.2.4 — 下屏改用 16px 全字集点阵，拉丁同源

> 注：这一版已在 v3.2.8 中被撤回，正文回到 12px。

- **中文换成 GNU Unifont 的 16px 原生点阵**（墨迹 15×16，填满整个字身框）。
  Unifont 完整覆盖 BMP，简体中文有 **20 992 个字形**，且 16px 档是
  **纯 1-bit 点阵**——所以 2bpp 图集格式和两个渲染后端都不用改。
- **拉丁也换成 Unifont 自己的半宽 8×16**，与中文同字体、同 em 框、同基线。
  此前下屏的 0-9 与 A-Z 取自游戏自带的 8×8 tile 字模（约 5×7 墨迹），
  和 16px 中文并排明显脱节。文本渲染因此**不再依赖游戏资源**（图标仍依赖）。
- **版面为 16px 重排**。3.2.3 的 y 坐标是按 ~10px 文字块调的，16px 块高了 60%，
  所以画面/开发者/设置/按键设置/叠加层/道具/读取存档/更新**八个页面**的行高
  与起始位置全部重算。横向不受影响：`text_width()` 按字符数计算
  （中文 16px、ASCII 8px），与字形大小无关。
- 字体与生成脚本见 [`fonts/README.md`](fonts/README.md)。

### v3.2.3 — 下屏字体与缩放

- **换成 12px 点阵字面**：每个字的墨迹从 9×9 增到 11×11，正好填满原先空着的
  上下空间。字身框仍是 16×16、文本进位仍是 16px（`text_width()` 按字符数计算，
  与字号无关），所以**版面坐标一处未改、无溢出风险**。
- **所有文本缩放改为整数倍**。此前面板标题是 1.5×、返回按钮 1.10×、更新横幅
  0.80× 等**非整数倍**，而采样是最近邻——1.5× 会让笔画粗细交替，0.80× 更是
  在**缩小丢像素**。现在统一为正文 1.0×、面板标题 2.0×。
- 字体与来源、校验和记录在 [`fonts/README.md`](fonts/README.md)，
  字形表可用 `tools/secondscreen/gen_cjk_atlas.py` 离线重新生成。

### v3.2.1 — 下屏文案补全

在 v3.2 的基础上，把下屏剩余未汉化的提示文案补齐（**开发者叠加层维持英文**，
便于对齐上游与排查问题）：

- 有可用更新时的横幅：`发现新版本 - 打开设置`；
- 设置 → `画面`：
  - `显示方式` 的值：原版 / 拉伸 / 宽屏；
  - `边缘模式` 的值：固定视角（此前只有「标准」是中文，现在两边一致）；
- 按键设置里等待输入时的 `请按键`；
- 开发者选项 → `读取存档`：值为 `等待` / `读取`，读取结果为
  `已读取` / `无转储` / `无存档` / `无效` / `版本不符` / `读写错误` / `错误`。

下屏字形表随之从 164 字扩到 176 字（`app/jni/src/src/ss_cjk_data.h`，
由 `tools/secondscreen/gen_cjk_atlas.py` 自动生成）。

> 有意保持英文的部分：手柄与按键名（`A` `B` `UP` `L1` …）、开发者叠加层的行标签
> （`VERSION` `MODEL` `FPS NOW` `ROOM` …）以及房间值（`HOUSE xx` / `OVERWORLD xx`）。

---

## 上游 3.2 更新内容

本 fork 已合并上游 **v3.2**（含 v3.2-E1、v3.2-E2）。相对 3.0E 共 124 个文件、+27964 / −785 行，主要新增：

- **Old 3DS 的 PICA200 GPU 渲染器** + 软件回退（老机型帧率明显提升）；
- 原生 **400×240 宽屏**输出；
- **游戏内更新器**（下屏 `更新` 项）：检查/下载/安装新版本，带 SHA-256 校验；
- 更新日志分页显示在**上屏**；
- 「重启」与「选择 ROM」合并在下屏设置中；
- 下屏新增 **读取存档（Load State）**、**显示帧率**、**内存转储** 等开发者选项；
- 触摸操作修正、新的横幅与 3D 图标。

**v3.2 相对 v3.2-E1 的两处修复（本次已同步）：**

- **更新器批量传输 SD 数据**：I/O 工作线程的优先级改为按调用者动态计算，
  下载不再被渲染任务饿死；
- 修复**重启 / 重选 ROM 后残留旧游戏 RAM** 导致的画面全黑、无声音。

> ⚠️ 上游更新器默认指向 **上游仓库**，那会把汉化版覆盖掉。本 fork 已把它改成本仓库
> （`platform/3ds/source/update_manifest.h` 的 `UPDATE_REPOSITORY`），只安装本项目的
> `zelda3-3ds-v3.2.8.cia / .3dsx`。
> 另注：上屏日志用的是 ASCII 定宽字体（`Update_FormatNotes` 会丢弃所有 ≥128 的字节），
> 因此本仓库的 Release 说明保持英文。

### 关于「更新」提示（有意如此）

版本号就是上游版本号，所以判断逻辑很简单：**版本号没变，游戏内不会提示更新**。

- 装的是 `v3.2.8`，重建同一版本 → 显示 **`已是最新版本`**，按钮只会做「检查更新」，不会重复下载安装；
- 只有版本号**变大**（上游发布更高的 `3.x`，或我们提升 `platform/3ds/CMakeLists.txt` 里的
  `ZELDA3_3DS_VERSION`）才会提示「发现新版本」并允许下载安装。

更新器会安装**和你当前所装一致的那个变体**：装的是标准版就装标准版，装的是 `-cn-bottom`
就装 `-cn-bottom`，不会因为更新把下屏中文弄丢（这一点从 v3.2.8 起修正，
更早的版本会把 `-cn-bottom` 覆盖成标准版）。

所以「同一版本重编译」的用户不会被反复打扰；要拿新构建直接从 Release 页下载即可
（同版本会**原地覆盖**那个 Release，所以 `/releases/latest` 永远指向当前发布的最新构建）。

---

## 下载（GitHub Release）

[**Releases**](https://github.com/frilxy/zelda-alttp-3ds-cn/releases) 里**每个版本一个 Release，永久保留**：

| 文件 | 用途 |
|---|---|
| `zelda3-3ds-v3.2.8.cia` | 标准版，FBI 安装（对话中文，下屏英文） |
| `zelda3-3ds-v3.2.8.3dsx` | 标准版，Homebrew Launcher |
| `zelda3-3ds-v3.2.8-cn-bottom.cia` | **下屏汉化版**，FBI 安装（下屏也中文） |
| `zelda3-3ds-v3.2.8-cn-bottom.3dsx` | **下屏汉化版**，Homebrew Launcher |
| `cn_language.bin` | 备用中文语言块（一般无需使用） |

发布规则：

- Release 的 tag 就是编译进程序的版本号（当前 `v3.2.8`）——游戏内更新器只认这种合法版本号，
  所以不使用 `latest` 之类的滚动 tag；`/releases/latest` 依旧指向最新版本。
- **版本号变化 → 新增一个 Release**（旧版本保留，可随时回退下载）。
- **同一版本重新构建 → 原地覆盖该版本的 Release**：资产与说明一并替换，tag 指向产生这些产物的那次提交；
  不会多出 Release，也不会动其它版本。

---

## 安装（CIA，推荐）

1. 用 FBI 安装下载的 `.cia`（想下屏中文就装 `-cn-bottom` 那个）。
2. 在 SD 卡创建目录 `sdmc:/3ds/Zelda 3DS/`。
3. 放入**干净美版 ROM**，命名为 `zelda3.sfc`（用于建立游戏 profile）。
   - ⚠️ **只放这一个**，不要放入其它汉化/修改版 ROM。
4. 音频固件 `sdmc:/3ds/dspfirm.cdc`（Luma3DS → Rosalina → Miscellaneous → Dump DSP firmware）。
5. 启动即可，**默认中文**，无需配置语言。

## 安装（3DSX）

1. 把 `.3dsx` 放到 `sdmc:/3ds/<某个文件夹>/`（Homebrew Launcher 可发现）。
2. 其余同上（`sdmc:/3ds/Zelda 3DS/` 放美版 ROM + `dspfirm.cdc`）。

---

## 构建

### GitHub Actions（自动）

每次 `push` / 手动触发，都会在 devkitARM 容器里**同时编译两个版本（标准 + 下屏汉化）**，并按上面的规则发布 Release（新版本新增、同版本覆盖）。

CI 会**从源码编译 `makerom`**（[3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR)）和 **`bannertool`**（[Epicpkmn11/bannertool](https://github.com/Epicpkmn11/bannertool)），因此不需要预装 devkitPro 工具。

更新器依赖的 `3ds-curl` / `3ds-mbedtls` / `3ds-jansson` 在 devkitARM 镜像里已自带（`/opt/devkitpro/portlibs/3ds`）；
若某天取不到，CI 会按 `platform/3ds/update-dependencies/` 里的版本、校验和与官方补丁
用 `build-deps.sh` **从源码交叉编译**一份作为兜底。

### 本地

需要 devkitARM（devkitPro）+ libctru + 3ds-cmake + `makerom` + `bannertool`：

```sh
chmod +x platform/3ds/build.sh
platform/3ds/build.sh                 # 标准版
BOTTOM_SCREEN_CN=1 platform/3ds/build.sh   # 下屏汉化版
```

产物在 `build-3ds/game/`（标准版）与 `build-3ds-cn-bottom/game/`（下屏汉化版）下。

---

## 操作

- 顶屏 400×240 游戏画面；底屏 320×240 即时地图 / 地牢地图 / 装备 / 道具 / 触碰设置。
- 显示模式：宽屏改版 / 拉伸原版 / 原版比例。
- 加速倍率：关闭 / ×2 / ×3 / ×4 / ×5（New 3DS 可用 ZL 或 C-Stick 按住）。
- 下屏设置：`画面` / `加速倍率` / `开发者选项` / `更新` / `重启`。
- 下屏 `更新`：检查并安装本仓库的最新版本（需 Wi-Fi），会安装与你当前所装一致的变体；
  点版本名可在**上屏**看更新日志。
- 快速诊断：按 `L + R + A` 生成内存 + 双屏截图转储。

## 致谢

上游 v3.2 的贡献者（随上游合并一并保留）：

- [@999sian](https://github.com/999sian)：Old 3DS 优化；
- [arth78](https://github.com/arth78)：WIDE 显示；
- [Archaistic](https://github.com/Archaistic)：WIDE 高度与 Triforce 改进；
- Logo：[Phibonacci](https://github.com/Phibonacci)，基于 [TiraArt](https://sketchfab.com/TiraArt) 的原始 3D 模型。

## 许可与法律

本项目的许可分几层，**这些组件许可都不授予任何任天堂游戏内容的权利**：

- **Zelda3 引擎**：沿用 **MIT 许可**与版权声明，见
  [`app/jni/src/LICENSE.txt`](app/jni/src/LICENSE.txt)（该文件同时含 Opus 的声明）；
- **SDL2 / SDL2_mixer 等依赖**：各自保留原有许可；
- **下屏中文字体**：[GNU Unifont](https://unifoundry.com/unifont/)，按 **SIL OFL 1.1**
  使用（该字体为 OFL 1.1 与 GPLv2+ 含字体嵌入例外 的双许可），
  见 [`fonts/unifont-LICENSE.txt`](fonts/unifont-LICENSE.txt)；
- **备用下屏字体**：[Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font)，
  SIL OFL 1.1，见 [`fonts/OFL.txt`](fonts/OFL.txt)。

仓库内容范围：

- 只含源码、构建脚本、可再分发移植资源与补丁/提取逻辑。
- 不包含 ROM、提取的游戏素材或未经授权的游戏数据。
- 请自行提供合法获得的美版 ROM。
