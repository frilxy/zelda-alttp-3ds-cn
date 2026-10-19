# Zelda: A Link to the Past 3DS — 简体中文版

Nintendo 3DS 双屏移植版《塞尔达传说：众神的三角力量》，本 fork **内置简体中文**。

基于以下开源工作：
- 逆向引擎 [snesrev/zelda3](https://github.com/snesrev/zelda3)
- 双屏 Android 基座 [Waterdish/zelda3-android](https://github.com/Waterdish/zelda3-android) / [samyost1/zelda3-android](https://github.com/samyost1/zelda3-android)
- 3DS 移植版 [EstebanPdN/zelda-alttp-3ds](https://github.com/EstebanPdN/zelda-alttp-3ds)
- 中文数据与字体：[LinkFish1/zelda3](https://github.com/LinkFish1/zelda3)、[Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font)（SIL OFL 开源）

本仓库**不包含 ROM**。你需要自备一份合法美版 ROM。

> ✅ 当前状态：**对话文本与下屏（第二屏）菜单/设置/地图 UI 全部中文化**，无需选择版本。

---

## 中文是怎么实现的

与上游“首次启动从 ROM 提取资源”不同，本 fork 采用 **LinkFish1 的“构建时烘焙”思路**：

- 把**已含中文的资源包 `zelda3_assets.dat`**（397 条中文对白 + 16×16 中文字体 + 字宽表 + `Language = cn` 标志）在构建时打包进 romfs；
- 移植了引擎字库渲染（`messaging.c` 的中文解码 + `VWF_RenderChinese`）；
- 下屏（第二屏）另外用 **10px Fusion Pixel 位图字体**渲染中文菜单（清晰、无描边）；
  字形表 `app/jni/src/src/ss_cjk_data.h` 由 `tools/secondscreen/gen_cjk_atlas.py`
  扫描下屏源码里的中文字符自动生成（当前 164 字），改文案后重跑该脚本即可；
- 更新器的状态提示（正在检查更新 / 下载校验失败 …）在下屏也一并中文化；
- 启动时直接复制内置资源，并强制 `Language = cn` → **默认中文**，无需配置。

> 因此游戏逻辑用的是内置资源包；SD 上的美版 ROM 仅用于**建立游戏 profile**，不会再从它提取/汉化。

---

## 说明

本 README 只讲怎么用；逐版本的改动记录见 [`CHANGELOG.md`](CHANGELOG.md)。

---

## 下载（GitHub Release）

[**Releases**](https://github.com/frilxy/zelda-alttp-3ds-cn/releases) 里**每个版本一个 Release，永久保留**：

| 文件 | 用途 |
|---|---|
| `zelda3-3ds-v3.2.4-cn-bottom.cia` | FBI 安装（推荐） |
| `zelda3-3ds-v3.2.4-cn-bottom.3dsx` | Homebrew Launcher |
| `cn_language.bin` | 备用中文语言块（一般无需使用） |

发布规则：

- Release 的 tag 就是编译进程序的版本号（当前 `v3.2.4`）——游戏内更新器只认这种合法版本号，
  所以不使用 `latest` 之类的滚动 tag；`/releases/latest` 依旧指向最新版本。
- **版本号变化 → 新增一个 Release**（旧版本保留，可随时回退下载）。
- **同一版本重新构建 → 原地覆盖该版本的 Release**：资产与说明一并替换，tag 指向产生这些产物的那次提交；
  不会多出 Release，也不会动其它版本。

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

## MSU-1 高音质音乐（可选）

本版支持 **MSU-1 流式音轨**，用 CD 音质音乐替换原版 SPC 音乐。

### 启用

把音轨放进 SD 卡：

```
sdmc:/3ds/Zelda 3DS/msu/
  ├── 1.pcm      ← 曲目编号 1..35
  ├── 2.pcm
  └── ...
```

**放好即可，无需改配置。** 启动时会自动探测 `msu/1.pcm`、`msu/0.pcm`、
`msu1.pcm`、`1.pcm`，命中就自动开启 MSU 并把输出切到 44100 Hz。

### 音轨格式

- **MSU-1 PCM**：44.1 kHz、16-bit、立体声
- 每个文件前 8 字节是 `"MSU1"` + repeat_position（小端 uint32）
- 文件名必须是**纯数字**（`1.pcm`，不能是 `alttp_msu-1.pcm`）

### 曲目编号

编号对应游戏场景，主流 MSU 包通用：

| 编号 | 场景 | 编号 | 场景 |
|---|---|---|---|
| 1 | 标题画面 | 19 | 击败加农 |
| 2 | 光之世界 | 20 | 教堂 |
| 3 | 开场雨景 | 21 | Boss 战 |
| 4 | 暗世界（兔子） | 22 | 暗世界地牢 |
| 5 | 迷途森林 | 23 | 占卜屋 |
| 6 | 开场故事 | 24 | 洞穴 |
| 7 | 卡卡利科村 | 25 | 救塞尔达 |
| 8 | 传送音效 | 26 | 贤者水晶 |
| 9 | 暗之世界 | 27 | 妖精泉 |
| 10 | 大师之剑 | 28 | 加农主题 |
| 11 | 存档画面 | 29 | 加农现影 |
| 12 | 士兵警报 | 30 | 最终对峙 |
| 13 | 斯卡尔森林 | 31 | 加农决战 |
| 14 | 迷你游戏 | 32-34 | 结局 |
| 15 | 未使用 | 35 | 视频音轨 |
| 16 | 海拉尔城堡 | | |
| 17 | 光之世界地牢 | | |
| 18 | 洞穴 | | |

> 缺少某号只会让那个场景回到原版音乐，不会报错。

### 手动配置（可选）

`zelda3.ini` 的 `[Sound]` 段：

```ini
MSUPath = msu/       # 音轨目录（相对 sdmc:/3ds/Zelda 3DS/，建议带尾斜杠）
EnableMSU = true     # true/1 = PCM；deluxe = Deluxe 变体
MSUVolume = 100%     # 混音音量
ResumeMSU = 0        # 1 = 离开再回场景时续播
```

- 不写 `MSUPath` 也可以：探测到音轨会自动补上目录。
- `opuz` 变体在 3DS 上会被忽略（它要求 48 kHz，本版固定 44.1 kHz）。

### 验证

启动后退出，在 `sdmc:/3ds/Zelda 3DS/runtime.log` 里找：

```
Audio config: 44100 Hz stereo, MSU on (enable=0x01, tracks found)
MSU: loading track 1 from msu/1.pcm
```

若出现 `MSU off (tracks not found)`，说明路径或文件名不对。

### 关于音轨来源

音轨包**不随本项目分发**（版权原因）。社区有多个免费包
（如 JUD6MENT 的 Orchestral 2023、TheRetromancer 的原曲风格管弦乐等），
可在 [Zeldix MSU1 数据库](https://www.zeldix.net/f55-the-legend-of-zelda-iii-a-link-to-the-past)
找到。

> 注意：标 **Deluxe** 的包需要给 ROM 打 ASM 补丁，本移植版**不支持**；
> 请用标准 MSU-1 包（33-35 首）。

---

## 构建

### GitHub Actions（自动）

每次 `push` / 手动触发都会在 devkitARM 容器里**编译唯一的那个构建**，并按上面的规则发布 Release（新版本新增、同版本覆盖）。
另有一个轻量 job 在普通 runner 上跑静态检查（头文件可见性、面板适配、图集是否与文案同步）。

CI 会**从源码编译 `makerom`**（[3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR)）和 **`bannertool`**（[Epicpkmn11/bannertool](https://github.com/Epicpkmn11/bannertool)），因此不需要预装 devkitPro 工具。

更新器依赖的 `3ds-curl` / `3ds-mbedtls` / `3ds-jansson` 在 devkitARM 镜像里已自带（`/opt/devkitpro/portlibs/3ds`）；
若某天取不到，CI 会按 `platform/3ds/update-dependencies/` 里的版本、校验和与官方补丁
用 `build-deps.sh` **从源码交叉编译**一份作为兜底。

### 本地

需要 devkitARM（devkitPro）+ libctru + 3ds-cmake + `makerom` + `bannertool`：

```sh
chmod +x platform/3ds/build.sh
platform/3ds/build.sh
```

产物在 `build-3ds/game/` 下。

---

## 操作

- 顶屏 400×240 游戏画面；底屏 320×240 即时地图 / 地牢地图 / 装备 / 道具 / 触碰设置。
- 显示模式：宽屏改版 / 拉伸原版 / 原版比例。
- 加速倍率：关闭 / ×2 / ×3 / ×4 / ×5（New 3DS 可用 ZL 或 C-Stick 按住）。
- 下屏设置：`画面` / `加速倍率` / `开发者选项` / `更新` / `重启`。
- 下屏 `更新`：检查并安装本仓库的最新版本（需 Wi-Fi）；
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
