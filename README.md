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
| **标准版** `zelda3-3ds-v3.2-E1` | 对话全中文，下屏仍为英文 |
| **下屏汉化版** `zelda3-3ds-v3.2-E1-cn-bottom` | 对话 + **下屏菜单/设置/地图 UI 全中文** |

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

## 上游 3.2-E1 更新内容

本 fork 已合并上游 **v3.2-E1**（相对 3.0E 共 124 个文件、+27964 / −785 行），主要新增：

- **Old 3DS 的 PICA200 GPU 渲染器** + 软件回退（老机型帧率明显提升）；
- 原生 **400×240 宽屏**输出；
- **游戏内更新器**（下屏 `更新` 项）：检查/下载/安装新版本，带 SHA-256 校验；
- 更新日志分页显示在**上屏**；
- 「重启」与「选择 ROM」合并在下屏设置中；
- 下屏新增 **读取存档（Load State）**、**显示帧率**、**内存转储** 等开发者选项；
- 触摸操作修正、新的横幅与 3D 图标。

> ⚠️ 上游更新器默认指向 **上游仓库**，那会把汉化版覆盖掉。本 fork 已把它改成本仓库
> （`platform/3ds/source/update_manifest.h` 的 `UPDATE_REPOSITORY`），只安装本项目的
> `zelda3-3ds-v3.2-E1.cia / .3dsx`。
> 另注：上屏日志用的是 ASCII 定宽字体（`Update_FormatNotes` 会丢弃所有 ≥128 的字节），
> 因此本仓库的 Release 说明保持英文。

---

## 下载（GitHub Release）

每次构建都会发布到 [**Releases**](https://github.com/frilxy/zelda-alttp-3ds-cn/releases/latest)，**只保留最新一次**：

| 文件 | 用途 |
|---|---|
| `zelda3-3ds-v3.2-E1.cia` | 标准版，FBI 安装（对话中文，下屏英文） |
| `zelda3-3ds-v3.2-E1.3dsx` | 标准版，Homebrew Launcher |
| `zelda3-3ds-v3.2-E1-cn-bottom.cia` | **下屏汉化版**，FBI 安装（下屏也中文） |
| `zelda3-3ds-v3.2-E1-cn-bottom.3dsx` | **下屏汉化版**，Homebrew Launcher |
| `cn_language.bin` | 备用中文语言块（一般无需使用） |

> Release 的 tag 就是编译进程序的版本号（当前 `v3.2-E1`）——游戏内更新器只认这种
> 合法版本号，所以不再使用 `latest` 之类的滚动 tag；`/releases/latest` 链接依旧可用。

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

每次 `push` / 手动触发，都会在 devkitARM 容器里**同时编译两个版本（标准 + 下屏汉化）**，并发布到 **GitHub Release（滚动最新）**。

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
- 下屏 `更新`：检查并安装本仓库的最新版本（需 Wi-Fi）；点版本名可在**上屏**看更新日志。
- 快速诊断：按 `L + R + A` 生成内存 + 双屏截图转储。

## 许可与法律

- 只含源码、构建脚本、可再分发移植资源与补丁/提取逻辑。
- 不包含 ROM、提取的游戏素材或未经授权的游戏数据。
- 请自行提供合法获得的美版 ROM。中文翻译与字体遵循其上游开源许可。
