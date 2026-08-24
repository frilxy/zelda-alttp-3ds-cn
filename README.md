# Zelda: A Link to the Past 3DS — 简体中文版

Nintendo 3DS 双屏移植版《塞尔达传说：众神的三角力量》，本 fork **内置简体中文支持**（对话文本全中文）。

基于以下开源工作：
- 逆向引擎 [snesrev/zelda3](https://github.com/snesrev/zelda3)
- 双屏 Android 基座 [Waterdish/zelda3-android](https://github.com/Waterdish/zelda3-android) / [samyost1/zelda3-android](https://github.com/samyost1/zelda3-android)
- 3DS 移植版 [EstebanPdN/zelda-alttp-3ds](https://github.com/EstebanPdN/zelda-alttp-3ds)
- 中文数据与字体：[LinkFish1/zelda3](https://github.com/LinkFish1/zelda3)（开源许可）

本仓库**不包含 ROM**。你需要自备一份合法美版 ROM。

> ⚠️ 当前状态：**对话文本已全部汉化**；**下屏菜单/设置（第二屏 UI）仍为英文**，后续可扩展。

---

## 中文是怎么实现的

与上游“首次启动从 ROM 提取资源”不同，本 fork 采用 **LinkFish1 的“构建时烘焙”思路**：

- 把**已含中文的资源包 `zelda3_assets.dat`**（397 条中文对白 + 16×16 中文字体 + 字宽表 + `Language = cn` 标志）在**构建时打包进 romfs**；
- 移植了引擎的字库渲染代码（`messaging.c` 的中文解码 + `VWF_RenderChinese`），能画 CJK；
- 启动时**直接复制内置资源**，并强制 `Language = cn` → **默认就是中文**，无需任何手动配置。

> 因此游戏逻辑使用的是**内置资源包**；SD 上的美版 ROM 仅用于**建立游戏 profile**，不会再从它提取/汉化。

---

## 下载（GitHub Release，滚动最新）

每次构建都会发布到 [**Releases**](https://github.com/frilxy/zelda-alttp-3ds-cn/releases/latest)，**只保留最新一次**：

| 文件 | 用途 |
|---|---|
| `zelda3-3ds-v2.9.2.cia` | FBI 安装（推荐） |
| `zelda3-3ds-v2.9.2.3dsx` | Homebrew Launcher 运行 |
| `cn_language.bin` | 备用中文语言块（一般无需使用） |

---

## 安装（CIA，推荐）

1. 用 FBI 安装下载的 `.cia`。
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

每次 `push` / 手动触发，都会在 devkitARM 容器里编译 **3DSX + CIA**，并发布到 **GitHub Release（滚动最新）**。

CI 会**从源码编译 `makerom`**（[3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR)）和 **`bannertool`**（[Epicpkmn11/bannertool](https://github.com/Epicpkmn11/bannertool)）+ 所需依赖，因此不需要预装 devkitPro 工具。

### 本地

需要 devkitARM（devkitPro）+ libctru + 3ds-cmake + `makerom` + `bannertool`：

```sh
chmod +x platform/3ds/build.sh
platform/3ds/build.sh
```

产物在 `build-3ds/game/` 下（`.cia` + `.3dsx`）。

---

## 操作

- 顶屏 400×240 游戏画面；底屏 320×240 即时地图 / 地牢地图 / 装备 / 道具 / 触碰设置。
- 显示模式：宽屏改版 / 拉伸原版 / 原版比例。
- 加速：关闭 / ×2 / ×3 / ×4 / ×5（New 3DS 可用 ZL 或 C-Stick 按住）。
- 快速诊断：按 `L + R + A` 生成内存 + 双屏截图转储。

## 许可与法律

- 只含源码、构建脚本、可再分发移植资源与补丁/提取逻辑。
- 不包含 ROM、提取的游戏素材或未经授权的游戏数据。
- 请自行提供合法获得的美版 ROM。中文翻译与字体遵循其上游开源许可。
