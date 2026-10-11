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
| **标准版** `zelda3-3ds-v3.2.2` | 对话全中文，下屏仍为英文 |
| **下屏汉化版** `zelda3-3ds-v3.2.2-cn-bottom` | 对话 + **下屏菜单/设置/地图 UI 全中文** |

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
> `zelda3-3ds-v3.2.2.cia / .3dsx`。
> 另注：上屏日志用的是 ASCII 定宽字体（`Update_FormatNotes` 会丢弃所有 ≥128 的字节），
> 因此本仓库的 Release 说明保持英文。

### 关于「更新」提示（有意如此）

版本号就是上游版本号，所以判断逻辑很简单：**版本号没变，游戏内不会提示更新**。

- 装的是 `v3.2.2`，重建同一版本 → 显示 **`已是最新版本`**，按钮只会做「检查更新」，不会重复下载安装；
- 只有版本号**变大**（上游发布更高的 `3.x`，或我们提升 `platform/3ds/CMakeLists.txt` 里的
  `ZELDA3_3DS_VERSION`）才会提示「发现新版本」并允许下载安装。

更新器会安装**和你当前所装一致的那个变体**：装的是标准版就装标准版，装的是 `-cn-bottom`
就装 `-cn-bottom`，不会因为更新把下屏中文弄丢（这一点从 v3.2.2 起修正，
更早的版本会把 `-cn-bottom` 覆盖成标准版）。

所以「同一版本重编译」的用户不会被反复打扰；要拿新构建直接从 Release 页下载即可
（同版本会**原地覆盖**那个 Release，所以 `/releases/latest` 永远指向当前发布的最新构建）。

---

## 下载（GitHub Release）

[**Releases**](https://github.com/frilxy/zelda-alttp-3ds-cn/releases) 里**每个版本一个 Release，永久保留**：

| 文件 | 用途 |
|---|---|
| `zelda3-3ds-v3.2.2.cia` | 标准版，FBI 安装（对话中文，下屏英文） |
| `zelda3-3ds-v3.2.2.3dsx` | 标准版，Homebrew Launcher |
| `zelda3-3ds-v3.2.2-cn-bottom.cia` | **下屏汉化版**，FBI 安装（下屏也中文） |
| `zelda3-3ds-v3.2.2-cn-bottom.3dsx` | **下屏汉化版**，Homebrew Launcher |
| `cn_language.bin` | 备用中文语言块（一般无需使用） |

发布规则：

- Release 的 tag 就是编译进程序的版本号（当前 `v3.2.2`）——游戏内更新器只认这种合法版本号，
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
- **下屏中文字体**：[Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font)，
  SIL OFL 1.1，见 [`fonts/OFL.txt`](fonts/OFL.txt)。

仓库内容范围：

- 只含源码、构建脚本、可再分发移植资源与补丁/提取逻辑。
- 不包含 ROM、提取的游戏素材或未经授权的游戏数据。
- 请自行提供合法获得的美版 ROM。
